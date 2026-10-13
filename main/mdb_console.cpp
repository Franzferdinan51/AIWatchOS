// MDB — Muse Developer Bridge, console half. Protocol: docs/mdb.md.
//
// A line-based command console on the USB serial port (the same port
// the boot log comes out of). Commands are prefixed `mdb `. The
// framebuffer dump separates buffer-truth from panel-truth when a
// display fault could live in either; the control commands inject
// input through the OS's own input seam (see mdb.hpp).
//
// Everything here compiles to nothing unless the firmware is built
// with CONFIG_AIWATCHOS_MDB_ENABLE (default ON for bring-up builds).
#include "mdb.hpp"
#include "aiwatchos/hal.hpp"   // FIRST: carries the ESP_PLATFORM -> __ESPRESSIF_IDF__ mapping (without it the guard below silently selects the stub start())

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if defined(__ESPRESSIF_IDF__) && defined(CONFIG_AIWATCHOS_MDB_ENABLE)

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>

#include "aiwatchos/weather_app.hpp"
#include "aiwatchos_hermes.hpp"
#include "aiwatchos_net/net.hpp"
#include "aiwatchos_weather/weather_service.hpp"
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace aiwatchos_mdb {
namespace {

// --- boot / panic retention -----------------------------------------
// RTC no-init memory survives warm resets (including panic resets), so
// the counters persist across the very crashes they count. Cold power
// loss reinitialises them via the magic check.
constexpr uint32_t kRtcMagic = 0x4D444231u;   // "MDB1"
RTC_NOINIT_ATTR uint32_t s_rtc_magic;
RTC_NOINIT_ATTR uint32_t s_boot_count;
RTC_NOINIT_ATTR uint32_t s_panic_count;

// --- log ring ---------------------------------------------------------
constexpr size_t kRingSize = 32 * 1024;
char* s_ring = nullptr;
size_t s_ring_head = 0;   // next write position
size_t s_ring_len = 0;    // valid bytes (<= kRingSize)
portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t s_prev_vprintf = nullptr;

int mdb_vprintf(const char* fmt, va_list ap) {
    if (s_ring) {
        char tmp[256];
        va_list aq;
        va_copy(aq, ap);
        const int n = vsnprintf(tmp, sizeof(tmp), fmt, aq);
        va_end(aq);
        if (n > 0) {
            const size_t w = static_cast<size_t>(n) < sizeof(tmp)
                                 ? static_cast<size_t>(n)
                                 : sizeof(tmp) - 1;
            portENTER_CRITICAL(&s_ring_mux);
            for (size_t i = 0; i < w; ++i) {
                s_ring[s_ring_head] = tmp[i];
                s_ring_head = (s_ring_head + 1) % kRingSize;
            }
            s_ring_len = (s_ring_len + w) < kRingSize ? s_ring_len + w
                                                     : kRingSize;
            portEXIT_CRITICAL(&s_ring_mux);
        }
    }
    return s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
}

// --- helpers ----------------------------------------------------------
const char* reset_reason_name(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "poweron";
        case ESP_RST_EXT: return "ext";
        case ESP_RST_SW: return "sw";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT: return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        default: return "other";
    }
}

// --- USB-Serial-JTAG transport ---------------------------------------
// The operator cable is the S3's native USB port, but this build's
// PRIMARY console is UART0 and USB-JTAG is only the log-mirroring
// secondary console — so stdio (getchar/printf) is bound to UART0 pins
// nothing is connected to, and a stdio-based MDB can neither hear nor
// answer the cable. MDB therefore talks to the USB-Serial-JTAG driver
// directly; the driver is installed in start(). (The log path still
// reaches the cable via the secondary console, which is why boot logs
// were visible while MDB stayed deaf.)
const char* kMdbTag = "aiw-mdb";

uint8_t s_rx[256];
size_t s_rx_pos = 0;
size_t s_rx_len = 0;

// Next byte from the cable, or -1 when none arrives within timeout_ms.
int usj_getchar(int timeout_ms) {
    if (s_rx_pos >= s_rx_len) {
        const int n = usb_serial_jtag_read_bytes(
            s_rx, sizeof(s_rx), pdMS_TO_TICKS(timeout_ms));
        if (n <= 0) return -1;
        s_rx_pos = 0;
        s_rx_len = static_cast<size_t>(n);
    }
    return s_rx[s_rx_pos++];
}

// Write all bytes to the cable. The driver copies each call into
// its TX ring buffer ALL-OR-NOTHING (xRingbufferSend): a call larger
// than the ring can never complete and just burns its timeout — the
// first live dump died exactly that way (4096 B chunks vs a 2048 B
// ring). Send in slices well under the ring size; each slice waits
// for the ISR to drain space toward the host. Bounded stalls: if no
// host is draining the port, give up instead of wedging the task.
bool usj_write_all(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    int stalls = 0;
    while (len > 0) {
        const size_t slice = len < 1024 ? len : 1024;
        const int n = usb_serial_jtag_write_bytes(
            p, slice, pdMS_TO_TICKS(2000));
        if (n > 0) {
            p += n;
            len -= static_cast<size_t>(n);
            stalls = 0;
        } else if (++stalls >= 5) {
            return false;
        }
    }
    return true;
}

bool read_line(char* out, size_t cap) {
    size_t n = 0;
    while (true) {
        const int c = usj_getchar(50);
        if (c < 0) {
            if (n == 0) return false;   // idle: caller loop re-enters
            continue;                   // mid-line pause: keep waiting
        }
        if (c == '\n' || c == '\r') {
            if (n == 0) continue;   // swallow CRLF pairs / leading blanks
            out[n] = '\0';
            return true;
        }
        if (n + 1 < cap) out[n++] = static_cast<char>(c);
    }
}

void out_str(const char* s) { usj_write_all(s, strlen(s)); }

// --- commands ---------------------------------------------------------
// Log silencer for binary transfers. esp_log_level_set("*", NONE)
// does NOT mute tags whose level is already in the log cache (every
// tag that has ever logged — the FT5x06 error pair interleaved into
// the first live dump and corrupted it). Every log line funnels
// through the vprintf hook, so swapping in a sink for the duration
// of the dump is airtight.
int mdb_sink_vprintf(const char* /*fmt*/, va_list /*ap*/) { return 0; }

void cmd_screenshot() {
    aiwatchos::Board& board = aiwatchos::Board::instance();
    uint16_t* fb = board.framebuffer();
    if (!fb) {
        out_str("ERR no framebuffer\n");
        return;
    }
    constexpr size_t kBytes = static_cast<size_t>(aiwatchos::kDisplayWidth) *
                              aiwatchos::kDisplayHeight * 2;
    // Keep interleaved log lines out of the binary stream: swap the
    // log vprintf for a sink (see mdb_sink_vprintf above) — setting
    // log levels does not mute tags already in the level cache.
    vprintf_like_t prev_vprintf = esp_log_set_vprintf(&mdb_sink_vprintf);
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "FBDUMP_BEGIN %d %d RGB565 %u\n",
             aiwatchos::kDisplayWidth, aiwatchos::kDisplayHeight,
             static_cast<unsigned>(kBytes));
    out_str(hdr);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(fb);
    size_t left = kBytes;
    bool ok = true;
    while (left > 0 && ok) {
        const size_t chunk = left > 4096 ? 4096 : left;
        ok = usj_write_all(p, chunk);
        p += chunk;
        left -= chunk;
        if (left > 0 && (left % (64 * 1024)) == 0) taskYIELD();
    }
    esp_log_set_vprintf(prev_vprintf);
    if (ok) out_str("\nFBDUMP_END\n");
}

void cmd_health() {
    aiwatchos::Board& board = aiwatchos::Board::instance();
    const aiwatchos::PowerStatus power = board.read_power();
    char buf[256];
    out_str("MDB_HEALTH_BEGIN\n");
    snprintf(buf, sizeof(buf), "uptime_s=%llu\n",
             static_cast<unsigned long long>(esp_timer_get_time() / 1000000));
    out_str(buf);
    snprintf(buf, sizeof(buf), "boot_count=%u panic_count=%u reset_reason=%s\n",
             static_cast<unsigned>(s_boot_count),
             static_cast<unsigned>(s_panic_count),
             reset_reason_name(esp_reset_reason()));
    out_str(buf);
    snprintf(buf, sizeof(buf), "heap_free=%u heap_min=%u psram_free=%u\n",
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(esp_get_minimum_free_heap_size()),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    out_str(buf);
    snprintf(buf, sizeof(buf), "battery_pct=%u battery_mv=%u charging=%d\n",
             power.battery_present ? power.battery_percent : 255,
             power.battery_mv, power.charging ? 1 : 0);
    out_str(buf);
    // WiFi: state + IP + RSSI.
    const aiwatchos_net::WifiState ws = aiwatchos_net::wifi_state();
    char ip[16] = "-";
    int rssi = 0;
    if (ws == aiwatchos_net::WifiState::kConnected) {
        if (esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_STA")) {
            esp_netif_ip_info_t info{};
            if (esp_netif_get_ip_info(sta, &info) == ESP_OK) {
                snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
            }
        }
        wifi_ap_record_t ap{};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
    }
    snprintf(buf, sizeof(buf), "wifi_state=%s wifi_ip=%s wifi_rssi=%d\n",
             aiwatchos_net::wifi_state_name(ws), ip, rssi);
    out_str(buf);
    snprintf(buf, sizeof(buf), "muse_link=%s\n",
             aiwatchos_net::muse_link_state_name(aiwatchos_net::muse_link_state()));
    out_str(buf);
    snprintf(buf, sizeof(buf), "hermes_paired=%d hermes_endpoint=lan\n",
             aiwatchos_hermes::hermes_paired() ? 1 : 0);
    out_str(buf);
    snprintf(buf, sizeof(buf), "hermes_health=%s\n",
             aiwatchos_hermes::hermes_health_line().c_str());
    out_str(buf);
    // Config PRESENCE only. Values never cross this console.
    const aiwatchos_net::NetConfig& cfg = aiwatchos_net::net_config();
    snprintf(buf, sizeof(buf),
             "cfg_wifi=%d cfg_hatch_token=%d cfg_vm_token=%d cfg_tts_key=%d\n",
             cfg.wifi_configured() ? 1 : 0, !cfg.hatch_token.empty() ? 1 : 0,
             !cfg.vm_token.empty() ? 1 : 0, !cfg.tts_key.empty() ? 1 : 0);
    out_str(buf);
    // Weather: status + age of the last-good snapshot.
    aiwatchos::WeatherSnapshot wx{};
    const bool have_wx = aiwatchos_weather::weather_service_snapshot(&wx);
    const char* wx_name = "unknown";
    if (have_wx) {
        switch (wx.status) {
            case aiwatchos::WeatherServiceStatus::kUnconfigured: wx_name = "unconfigured"; break;
            case aiwatchos::WeatherServiceStatus::kConnecting: wx_name = "connecting"; break;
            case aiwatchos::WeatherServiceStatus::kOk: wx_name = "ok"; break;
            case aiwatchos::WeatherServiceStatus::kFetchFailed: wx_name = "fetch_failed"; break;
        }
    }
    long wx_age_s = -1;
    if (have_wx && wx.has_data) {
        const time_t now = time(nullptr);
        if (now > 1600000000) {
            wx_age_s = static_cast<long>(now - static_cast<time_t>(wx.fetched_epoch_ms / 1000));
        }
    }
    snprintf(buf, sizeof(buf), "weather_status=%s weather_age_s=%ld current_app=%s asleep=%d\n",
             wx_name, wx_age_s,
             current_app_id() ? current_app_id() : "?",
             display_asleep() ? 1 : 0);
    out_str(buf);
    out_str("MDB_HEALTH_END\n");
}

void cmd_logs() {
    if (!s_ring) {
        out_str("ERR log ring unavailable\n");
        return;
    }
    char hdr[64];
    portENTER_CRITICAL(&s_ring_mux);
    const size_t len = s_ring_len;
    const size_t start = (s_ring_head + kRingSize - len) % kRingSize;
    portEXIT_CRITICAL(&s_ring_mux);
    snprintf(hdr, sizeof(hdr), "MDB_LOGS_BEGIN %u\n", static_cast<unsigned>(len));
    out_str(hdr);
    char chunk[512];
    size_t done = 0;
    while (done < len) {
        size_t n = len - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        portENTER_CRITICAL(&s_ring_mux);
        for (size_t i = 0; i < n; ++i) {
            chunk[i] = s_ring[(start + done + i) % kRingSize];
        }
        portEXIT_CRITICAL(&s_ring_mux);
        if (!usj_write_all(chunk, n)) return;
        done += n;
    }
    out_str("\nMDB_LOGS_END\n");
}

void cmd_touch() {
    aiwatchos::Board& board = aiwatchos::Board::instance();
    out_str("MDB_TOUCH_BEGIN send-any-byte-to-stop\n");
    int last_n = 0;
    while (true) {
        if (usj_getchar(0) >= 0) break;   // any byte stops the stream
        int xs[2] = {0}, ys[2] = {0};
        const int n = board.read_touch(xs, ys);
        if (n != last_n || n > 0) {
            char buf[64];
            if (n > 0) {
                snprintf(buf, sizeof(buf), "TOUCH %d %d %d\n", n, xs[0], ys[0]);
            } else {
                snprintf(buf, sizeof(buf), "TOUCH_UP\n");
            }
            out_str(buf);
            last_n = n;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    out_str("MDB_TOUCH_END\n");
}

void cmd_panic() {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "reset_reason=%s boot_count=%u panic_count=%u\n",
             reset_reason_name(esp_reset_reason()),
             static_cast<unsigned>(s_boot_count),
             static_cast<unsigned>(s_panic_count));
    out_str("MDB_PANIC_BEGIN\n");
    out_str(buf);
    out_str("MDB_PANIC_END\n");
}

void cmd_help() {
    out_str(
        "MDB — Muse Developer Bridge (docs/mdb.md)\n"
        "  mdb screenshot            framebuffer dump (FBDUMP_BEGIN..END)\n"
        "  mdb health                state snapshot (key=value)\n"
        "  mdb logs                  recent log ring (~32 KB)\n"
        "  mdb touch                 stream touch points until any byte\n"
        "  mdb panic                 reset reason + boot/panic counters\n"
        "  mdb tap <x> <y>           inject a tap\n"
        "  mdb swipe <x1> <y1> <x2> <y2> [ms]\n"
        "  mdb press <boot|pwr>     inject a button press\n"
        "  mdb goto <app-id>         switch app (clock|launcher|muse|hermes|settings|notifications|weather)\n"
        "  mdb wake | mdb sleep | mdb reboot\n");
}

void dispatch(char* line) {
    // Tokenise on spaces; commands are exactly `mdb <verb> [args]`.
    char* save = nullptr;
    const char* tok = strtok_r(line, " \t", &save);
    if (!tok || strcmp(tok, "mdb") != 0) return;   // not ours; stay silent
    const char* verb = strtok_r(nullptr, " \t", &save);
    if (!verb) {
        cmd_help();
        return;
    }
    if (strcmp(verb, "help") == 0) { cmd_help(); return; }
    if (strcmp(verb, "screenshot") == 0) { cmd_screenshot(); return; }
    if (strcmp(verb, "health") == 0) { cmd_health(); return; }
    if (strcmp(verb, "logs") == 0) { cmd_logs(); return; }
    if (strcmp(verb, "touch") == 0) { cmd_touch(); return; }
    if (strcmp(verb, "panic") == 0) { cmd_panic(); return; }
    if (strcmp(verb, "reboot") == 0) {
        out_str("OK rebooting\n");
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
        return;
    }
    if (strcmp(verb, "wake") == 0) { request_wake(); out_str("OK wake\n"); return; }
    if (strcmp(verb, "sleep") == 0) { request_sleep(); out_str("OK sleep\n"); return; }
    if (strcmp(verb, "tap") == 0) {
        const char* a = strtok_r(nullptr, " \t", &save);
        const char* b = strtok_r(nullptr, " \t", &save);
        if (!a || !b) { out_str("ERR usage: mdb tap <x> <y>\n"); return; }
        inject_tap(atoi(a), atoi(b));
        out_str("OK tap\n");
        return;
    }
    if (strcmp(verb, "swipe") == 0) {
        int v[5] = {0, 0, 0, 0, 300};
        for (int i = 0; i < 5; ++i) {
            const char* a = strtok_r(nullptr, " \t", &save);
            if (!a) {
                if (i < 4) { out_str("ERR usage: mdb swipe <x1> <y1> <x2> <y2> [ms]\n"); return; }
                break;
            }
            v[i] = atoi(a);
        }
        inject_swipe(v[0], v[1], v[2], v[3], v[4]);
        out_str("OK swipe\n");
        return;
    }
    if (strcmp(verb, "press") == 0) {
        const char* a = strtok_r(nullptr, " \t", &save);
        if (a && strcmp(a, "boot") == 0) { inject_button(0x01); out_str("OK press boot\n"); return; }
        if (a && strcmp(a, "pwr") == 0) { inject_button(0x02); out_str("OK press pwr\n"); return; }
        out_str("ERR press: only boot|pwr are hardware buttons; PTT is an on-screen target — use mdb tap\n");
        return;
    }
    if (strcmp(verb, "goto") == 0 || strcmp(verb, "open") == 0) {
        const char* a = strtok_r(nullptr, " \t", &save);
        if (!a) { out_str("ERR usage: mdb goto <app-id>\n"); return; }
        request_switch_app(a);
        out_str("OK goto queued\n");
        return;
    }
    out_str("ERR unknown mdb command — 'mdb help'\n");
}

void mdb_task(void*) {
    out_str("MDB ready — 'mdb help'\n");
    char line[128];
    while (true) {
        if (read_line(line, sizeof(line))) dispatch(line);
    }
}

}  // namespace

void start() {
    static bool s_started = false;
    if (s_started) return;
    s_started = true;

    if (s_rtc_magic != kRtcMagic) {
        s_boot_count = 0;
        s_panic_count = 0;
        s_rtc_magic = kRtcMagic;
    }
    ++s_boot_count;
    if (esp_reset_reason() == ESP_RST_PANIC) ++s_panic_count;

    // Install the USB-Serial-JTAG driver behind the MDB transport
    // (see the transport note at usj_getchar). ESP_ERR_INVALID_STATE
    // means the driver is already up — the read/write calls share the
    // same driver object and work identically either way.
    usb_serial_jtag_driver_config_t usj_cfg = {
        .tx_buffer_size = 8192,
        .rx_buffer_size = 2048,
    };
    const esp_err_t usj_err = usb_serial_jtag_driver_install(&usj_cfg);
    if (usj_err != ESP_OK && usj_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kMdbTag, "USB-Serial-JTAG install failed: %s",
                 esp_err_to_name(usj_err));
    }

    s_ring = static_cast<char*>(heap_caps_malloc(kRingSize, MALLOC_CAP_SPIRAM));
    if (s_ring) {
        s_prev_vprintf = esp_log_set_vprintf(&mdb_vprintf);
    }
    xTaskCreate(&mdb_task, "mdb", 6144, nullptr, 5, nullptr);
    ESP_LOGI(kMdbTag, "MDB ready — USB-Serial-JTAG transport, 'mdb help'");
}

}  // namespace aiwatchos_mdb

#else  // MDB disabled or host build: start() is a no-op.

namespace aiwatchos_mdb {
void start() {}
}  // namespace aiwatchos_mdb

#endif
