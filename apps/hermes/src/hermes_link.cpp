// Hermes link layer — everything ESP-IDF in the Hermes app lives here:
// NVS identity, the WebSocket to the Hermes hub, the OTA flash backend,
// and the pump task that feeds frames into the protocol client
// (hermes_proto.cpp) via the hermes_core_* entry points in the adapter.
//
// WiFi itself is NOT owned here: components/aiwatchos_net (net_start() in
// app_main) is the single WiFi/SNTP owner. This layer only observes
// aiwatchos_net::wifi_connected() and runs the WebSocket while the shared
// STA link is up; credentials come from the shared net config (Kconfig /
// NVS namespace "aiwnet"), never from Hermes-private settings.
//
// Memory discipline (ESP32-S3: internal SRAM is the scarce resource):
//   - the protocol client object and this layer's frame buffers live in
//     PSRAM via explicit heap_caps; allocations fail closed (drop/stay
//     offline) rather than falling back to internal RAM;
//   - cJSON's allocator hooks are pointed at PSRAM, so parsing a large
//     frame never touches DRAM;
//   - the pump task stack is 6144 bytes — sized for flat JSON parsing and
//     frame copies; the heavy lifting (WiFi, TCP, WS framing) runs in
//     system/library tasks, not here.
//
// The host build compiles the stub section at the bottom instead: no
// transport, an in-memory OTA capture sink, deterministic randomness.
// FIRST: hal.hpp carries the ESP_PLATFORM -> __ESPRESSIF_IDF__ mapping.
// Without it this TU silently compiled the host-stub branch into device
// firmware (found by the buddy harvest: the ELF had no pump task at all).
#include "aiwatchos/hal.hpp"

#include "hermes_internal.hpp"
#include "hermes_proto.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifdef __ESPRESSIF_IDF__

#include "aiwatchos/hal.hpp"
#include "aiwatchos_net/net.hpp"   // wifi_connected(), net_config() — shared net owner
#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace aiwatchos_hermes {
namespace {

const char* TAG = "hermes-link";

#ifndef CONFIG_HERMES_HOST
#define CONFIG_HERMES_HOST "192.168.1.96"
#endif
#ifndef CONFIG_HERMES_PORT
#define CONFIG_HERMES_PORT 8765
#endif

constexpr size_t kMaxMessage = 16384;   // bounds one reassembled WS message
constexpr int kPumpStackBytes = 6144;
constexpr uint32_t kBackoffMinMs = 1000;
constexpr uint32_t kBackoffMaxMs = 30000;

bool g_nvs_ready = false;
volatile bool g_wake = false;   // reconnect-wakeup poke for the pump loop
bool g_started = false;
bool g_ws_up = false;
bool g_ever_connected = false;
uint32_t g_backoff_ms = kBackoffMinMs;
int64_t g_next_attempt_ms = 0;
esp_websocket_client_handle_t g_ws = nullptr;

// Reassembly buffer for fragmented WS messages (PSRAM, allocated once).
uint8_t* g_rx = nullptr;
size_t g_rx_len = 0;
uint8_t g_rx_opcode = 0;

struct Msg {
    enum Kind : uint8_t { Up, Down, Text, Binary } kind;
    uint8_t* data;   // PSRAM, owned by the queue consumer (Text/Binary)
    size_t len;
};
QueueHandle_t g_queue = nullptr;

int64_t now_ms64() { return static_cast<int64_t>(aiwatchos::hal().now_ms()); }

void post(Msg::Kind kind, uint8_t* data, size_t len) {
    if (!g_queue) {
        if (data) heap_caps_free(data);
        return;
    }
    Msg* m = static_cast<Msg*>(heap_caps_malloc(sizeof(Msg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!m) {
        if (data) heap_caps_free(data);
        return;
    }
    m->kind = kind;
    m->data = data;
    m->len = len;
    if (xQueueSend(g_queue, &m, 0) != pdTRUE) {
        if (data) heap_caps_free(data);
        heap_caps_free(m);
    }
}

bool ensure_nvs() {
    if (g_nvs_ready) return true;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    g_nvs_ready = (err == ESP_OK);
    if (!g_nvs_ready) ESP_LOGW(TAG, "NVS init failed: %s", esp_err_to_name(err));
    return g_nvs_ready;
}

// --- WebSocket events (run on the WS client's task) -------------------------

void ws_event_handler(void*, esp_event_base_t, int32_t event_id, void* event_data) {
    auto* d = static_cast<esp_websocket_event_data_t*>(event_data);
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            g_ws_up = true;
            g_ever_connected = true;
            g_backoff_ms = kBackoffMinMs;
            post(Msg::Up, nullptr, 0);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
        case WEBSOCKET_EVENT_ERROR:
            if (g_ws_up) post(Msg::Down, nullptr, 0);
            g_ws_up = false;
            g_rx_len = 0;
            break;
        case WEBSOCKET_EVENT_DATA: {
            uint8_t op = d->op_code;
            if (op == 0x8 || op == 0x9 || op == 0xA) break;   // close/ping/pong: client handles
            if (op != 0x0) g_rx_opcode = op;
            if (d->payload_offset == 0 && op != 0x0) g_rx_len = 0;
            if (!g_rx) break;
            if (g_rx_len + static_cast<size_t>(d->data_len) > kMaxMessage) {
                ESP_LOGW(TAG, "dropping oversized message");
                g_rx_len = 0;
                break;
            }
            std::memcpy(g_rx + g_rx_len, d->data_ptr, static_cast<size_t>(d->data_len));
            g_rx_len += static_cast<size_t>(d->data_len);
            bool done = d->payload_offset + d->data_len >= d->payload_len;
            if (done && d->fin) {
                uint8_t* copy = static_cast<uint8_t*>(
                    heap_caps_malloc(g_rx_len ? g_rx_len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (copy) {
                    std::memcpy(copy, g_rx, g_rx_len);
                    post(g_rx_opcode == 0x2 ? Msg::Binary : Msg::Text, copy, g_rx_len);
                }
                g_rx_len = 0;
            }
            break;
        }
        default:
            break;
    }
}

void ws_start() {
    if (g_ws) {
        esp_websocket_client_destroy(g_ws);
        g_ws = nullptr;
    }
    char uri[128];
    std::snprintf(uri, sizeof(uri), "ws://%s:%d/gadget", CONFIG_HERMES_HOST, CONFIG_HERMES_PORT);
    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri;
    cfg.subprotocol = "hermes-gadget.v1";
    cfg.buffer_size = 4096;
    cfg.task_stack = 6144;
    cfg.disable_auto_reconnect = true;   // this layer owns backoff
    cfg.network_timeout_ms = 10000;
    cfg.ping_interval_sec = 0;           // protocol heartbeat instead
    g_ws = esp_websocket_client_init(&cfg);
    if (!g_ws) return;
    esp_websocket_register_events(g_ws, WEBSOCKET_EVENT_ANY, &ws_event_handler, nullptr);
    if (esp_websocket_client_start(g_ws) != ESP_OK) {
        esp_websocket_client_destroy(g_ws);
        g_ws = nullptr;
    }
}


// --- pump task ---------------------------------------------------------------

void pump_task(void*) {
    bool mic_on = false;
    uint64_t last_battery_ms = 0;
    int16_t mic_buf[320];
    for (;;) {
        Msg* m = nullptr;
        if (g_queue && xQueueReceive(g_queue, &m, pdMS_TO_TICKS(30)) == pdTRUE && m) {
            switch (m->kind) {
                case Msg::Up: hermes_core_transport_up(); break;
                case Msg::Down: hermes_core_transport_down(); break;
                case Msg::Text:
                    if (m->data) hermes_core_transport_text(reinterpret_cast<const char*>(m->data), m->len);
                    break;
                case Msg::Binary:
                    if (m->data) hermes_core_transport_binary(m->data, m->len);
                    break;
            }
            if (m->data) heap_caps_free(m->data);
            heap_caps_free(m);
        }

        // Buddy-harvest wakeup: a poke (app foregrounded, text queued,
        // identity forgotten) cuts the current backoff wait short —
        // the pump loop polls at <=30 ms, so the retry is immediate.
        if (g_wake) {
            g_wake = false;
            g_next_attempt_ms = 0;
            g_backoff_ms = kBackoffMinMs;
        }

        // (Re)connect management with exponential backoff. After an auth
        // failure the client says wants_reconnect()==false and we stop
        // until the owner forgets the device (app gesture or host-side).
        int64_t now = now_ms64();
        if (aiwatchos_net::wifi_connected() && !g_ws_up && now >= g_next_attempt_ms &&
            (!g_ever_connected || hermes_core_wants_reconnect())) {
            ws_start();
            g_next_attempt_ms = now + g_backoff_ms;
            g_backoff_ms = g_backoff_ms * 2 > kBackoffMaxMs ? kBackoffMaxMs : g_backoff_ms * 2;
        }

        hermes_core_pump();

        // Battery telemetry (the client throttles the actual sends).
        uint64_t unow = aiwatchos::hal().now_ms();
        if (unow - last_battery_ms >= 2000) {
            last_battery_ms = unow;
            aiwatchos::PowerStatus ps = aiwatchos::hal().read_power();
            hermes_core_set_battery(ps.battery_percent <= 100 ? ps.battery_percent : -1,
                                    ps.battery_present ? (ps.charging ? 1 : 0) : -1);
        }

        // Push-to-talk mic pump while a capture is live.
        if (hermes_core_talk_active()) {
            if (!mic_on) mic_on = aiwatchos::hal().mic_start(16000);
            if (mic_on) {
                size_t got = aiwatchos::hal().mic_read(mic_buf, 320);
                if (got) hermes_core_talk_feed(mic_buf, got);
            }
        } else if (mic_on) {
            aiwatchos::hal().mic_stop();
            mic_on = false;
        }

        // OTA just completed: the client asked for a reboot into the new
        // firmware (its ota.done is already on the wire).
        if (hermes_core_ota_reboot_pending()) {
            hermes_core_ota_reboot_consumed();
            vTaskDelay(pdMS_TO_TICKS(1200));
            esp_restart();
        }
    }
}

// --- OTA flash backend ---------------------------------------------------------

const esp_partition_t* g_ota_part = nullptr;
esp_ota_handle_t g_ota_handle = 0;
bool g_ota_active = false;

}  // namespace

bool hermes_link_load_key(uint8_t key[32]) {
    if (!ensure_nvs()) return false;
    nvs_handle_t h;
    if (nvs_open("hermes", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 32;
    esp_err_t err = nvs_get_blob(h, "devkey", key, &len);
    nvs_close(h);
    return err == ESP_OK && len == 32;
}

void hermes_link_store_key(const uint8_t key[32]) {
    if (!ensure_nvs()) return;
    nvs_handle_t h;
    if (nvs_open("hermes", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "devkey", key, 32);
    nvs_commit(h);
    nvs_close(h);
}

void hermes_link_erase_key() {
    if (!ensure_nvs()) return;
    nvs_handle_t h;
    if (nvs_open("hermes", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "devkey");
    nvs_commit(h);
    nvs_close(h);
}

void hermes_link_wake_reconnect() { g_wake = true; }

void hermes_link_save_outbox(const char* const* texts, int count) {
    if (!ensure_nvs()) return;
    if (count < 0) count = 0;
    if (count > 4) count = 4;
    nvs_handle_t h;
    if (nvs_open("hermes", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "qn", static_cast<uint8_t>(count));
    for (int i = 0; i < count; ++i) {
        char key[3] = {'q', static_cast<char>('0' + i), '\0'};
        const char* t = texts[i] ? texts[i] : "";
        nvs_set_blob(h, key, t, std::strlen(t) + 1);
    }
    nvs_commit(h);
    nvs_close(h);
}

int hermes_link_load_outbox(char (*out)[512], int max) {
    if (!ensure_nvs() || !out || max <= 0) return 0;
    nvs_handle_t h;
    if (nvs_open("hermes", NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t n = 0;
    if (nvs_get_u8(h, "qn", &n) != ESP_OK) n = 0;
    int count = 0;
    for (int i = 0; i < n && i < max && i < 4; ++i) {
        char key[3] = {'q', static_cast<char>('0' + i), '\0'};
        size_t len = 512;
        if (nvs_get_blob(h, key, out[i], &len) == ESP_OK && len > 0) {
            out[i][511] = '\0';
            if (out[i][0]) ++count;
        }
    }
    nvs_close(h);
    return count;
}

void hermes_link_save_conversation(const char* transcript, const char* reply) {
    if (!ensure_nvs()) return;
    nvs_handle_t h;
    if (nvs_open("hermes", NVS_READWRITE, &h) != ESP_OK) return;
    const char* t = transcript ? transcript : "";
    const char* r = reply ? reply : "";
    nvs_set_blob(h, "ct", t, std::strlen(t) + 1);
    nvs_set_blob(h, "cr", r, std::strlen(r) + 1);
    nvs_commit(h);
    nvs_close(h);
}

bool hermes_link_load_conversation(char* t, size_t tcap, char* r, size_t rcap) {
    if (!ensure_nvs() || !t || !r || tcap == 0 || rcap == 0) return false;
    nvs_handle_t h;
    if (nvs_open("hermes", NVS_READONLY, &h) != ESP_OK) return false;
    size_t tl = tcap, rl = rcap;
    bool ok = nvs_get_blob(h, "ct", t, &tl) == ESP_OK &&
              nvs_get_blob(h, "cr", r, &rl) == ESP_OK;
    nvs_close(h);
    if (ok) {
        t[tcap - 1] = '\0';
        r[rcap - 1] = '\0';
    }
    return ok;
}

void hermes_link_clear_conversation() {
    if (!ensure_nvs()) return;
    nvs_handle_t h;
    if (nvs_open("hermes", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "ct");
    nvs_erase_key(h, "cr");
    nvs_commit(h);
    nvs_close(h);
}

void hermes_link_fill_random(uint8_t* out, size_t len) { esp_fill_random(out, len); }

bool hermes_link_start() {
    if (g_started) return true;
    g_started = true;
    ensure_nvs();
    // cJSON allocates through PSRAM for this component (fail closed).
    static cJSON_Hooks hooks{[](size_t n) -> void* {
                                 return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                             },
                             [](void* p) { heap_caps_free(p); }};
    cJSON_InitHooks(&hooks);
    g_rx = static_cast<uint8_t*>(heap_caps_malloc(kMaxMessage, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_queue = xQueueCreate(8, sizeof(Msg*));
    if (!g_rx || !g_queue) {
        ESP_LOGE(TAG, "PSRAM alloc failed — Hermes link disabled");
        return false;
    }
    // No WiFi bring-up here (single owner: aiwatchos_net). The pump task
    // below idles offline until the shared link reports connected; an
    // unconfigured net is a valid, offline-by-config outcome.
    if (!aiwatchos_net::net_config().wifi_configured()) {
        ESP_LOGW(TAG, "shared net has no WiFi credentials - Hermes stays offline");
    }
    return xTaskCreate(&pump_task, "hermes_link", kPumpStackBytes, nullptr, 5, nullptr) == pdPASS;
}

bool hermes_link_send_text(const char* json, size_t len) {
    if (!g_ws || !esp_websocket_client_is_connected(g_ws)) return false;
    return esp_websocket_client_send_text(g_ws, json, static_cast<int>(len),
                                          pdMS_TO_TICKS(2000)) == static_cast<int>(len);
}

bool hermes_link_send_binary(const uint8_t* data, size_t len) {
    if (!g_ws || !esp_websocket_client_is_connected(g_ws)) return false;
    return esp_websocket_client_send_bin(g_ws, reinterpret_cast<const char*>(data),
                                         static_cast<int>(len),
                                         pdMS_TO_TICKS(2000)) == static_cast<int>(len);
}

bool hermes_link_ota_begin(uint32_t size) {
    g_ota_part = esp_ota_get_next_update_partition(nullptr);
    if (!g_ota_part) return false;
    // Two-sided validation: the advertised protocol cap (kHermesOtaMaxSize,
    // enforced again in the protocol core at offer time) AND the actual
    // size of the next update partition, read at runtime — the integrated
    // partition table gives 2 MB slots (2,097,152 B), above the protocol
    // cap (2,031,616 B), so the cap binds first; nothing here hardcodes a
    // slot size.
    if (size > kHermesOtaMaxSize) return false;
    if (size > g_ota_part->size) return false;
    if (esp_ota_begin(g_ota_part, size, &g_ota_handle) != ESP_OK) return false;
    g_ota_active = true;
    return true;
}

bool hermes_link_ota_write(const uint8_t* data, size_t len) {
    if (!g_ota_active) return false;
    return esp_ota_write(g_ota_handle, data, len) == ESP_OK;
}

bool hermes_link_ota_finish() {
    if (!g_ota_active) return false;
    g_ota_active = false;
    if (esp_ota_end(g_ota_handle) != ESP_OK) return false;
    return esp_ota_set_boot_partition(g_ota_part) == ESP_OK;
}

void hermes_link_ota_abort() {
    if (g_ota_active) {
        esp_ota_abort(g_ota_handle);
        g_ota_active = false;
    }
}

void* hermes_link_alloc_big(size_t len) {
    return heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // no internal fallback
}

void hermes_link_free_big(void* ptr) { heap_caps_free(ptr); }

size_t hermes_link_test_ota_size() { return 0; }
const uint8_t* hermes_link_test_ota_data() { return nullptr; }
void hermes_link_test_ota_reset() {}

}  // namespace aiwatchos_hermes

#else  // host stubs

#include <vector>

namespace aiwatchos_hermes {
namespace {
std::vector<uint8_t> g_ota_capture;
bool g_ota_active = false;
uint32_t g_rand_state = 0x2545F491;
}  // namespace

bool hermes_link_start() { return false; }
bool hermes_link_send_text(const char*, size_t) { return false; }
bool hermes_link_send_binary(const uint8_t*, size_t) { return false; }
bool hermes_link_load_key(uint8_t[32]) { return false; }
void hermes_link_store_key(const uint8_t[32]) {}
void hermes_link_erase_key() {}
void hermes_link_wake_reconnect() {}

// In-memory persistence stubs: same semantics as the NVS layer, so the
// adapter's save/restore flows run identically in host tests.
namespace {
char g_stub_outbox[4][512] = {};
int g_stub_outbox_n = 0;
char g_stub_ct[512] = {};
char g_stub_cr[4096] = {};
bool g_stub_conv = false;

void stub_copy(char* dst, size_t cap, const char* src) {
    if (!src) src = "";
    size_t n = std::strlen(src);
    if (n >= cap) n = cap - 1;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}
}  // namespace

void hermes_link_save_outbox(const char* const* texts, int count) {
    if (count < 0) count = 0;
    if (count > 4) count = 4;
    g_stub_outbox_n = count;
    for (int i = 0; i < count; ++i) stub_copy(g_stub_outbox[i], 512, texts[i]);
}

int hermes_link_load_outbox(char (*out)[512], int max) {
    int n = g_stub_outbox_n < max ? g_stub_outbox_n : max;
    for (int i = 0; i < n; ++i) stub_copy(out[i], 512, g_stub_outbox[i]);
    return n;
}

void hermes_link_save_conversation(const char* transcript, const char* reply) {
    stub_copy(g_stub_ct, sizeof(g_stub_ct), transcript);
    stub_copy(g_stub_cr, sizeof(g_stub_cr), reply);
    g_stub_conv = true;
}

bool hermes_link_load_conversation(char* t, size_t tcap, char* r, size_t rcap) {
    if (!g_stub_conv) return false;
    stub_copy(t, tcap, g_stub_ct);
    stub_copy(r, rcap, g_stub_cr);
    return true;
}

void hermes_link_clear_conversation() {
    g_stub_conv = false;
    g_stub_ct[0] = '\0';
    g_stub_cr[0] = '\0';
}
void hermes_link_fill_random(uint8_t* out, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        g_rand_state = g_rand_state * 1103515245u + 12345u;
        out[i] = static_cast<uint8_t>(g_rand_state >> 16);
    }
}
bool hermes_link_ota_begin(uint32_t size) {
    g_ota_capture.clear();
    g_ota_capture.reserve(size);
    g_ota_active = true;
    return true;
}
bool hermes_link_ota_write(const uint8_t* data, size_t len) {
    if (!g_ota_active) return false;
    g_ota_capture.insert(g_ota_capture.end(), data, data + len);
    return true;
}
bool hermes_link_ota_finish() {
    if (!g_ota_active) return false;
    g_ota_active = false;
    return true;
}
void hermes_link_ota_abort() { g_ota_active = false; }
void* hermes_link_alloc_big(size_t len) { return std::malloc(len); }
void hermes_link_free_big(void* ptr) { std::free(ptr); }
size_t hermes_link_test_ota_size() { return g_ota_capture.size(); }
const uint8_t* hermes_link_test_ota_data() {
    return g_ota_capture.empty() ? nullptr : g_ota_capture.data();
}
void hermes_link_test_ota_reset() {
    g_ota_capture.clear();
    g_ota_active = false;
}

}  // namespace aiwatchos_hermes

#endif
