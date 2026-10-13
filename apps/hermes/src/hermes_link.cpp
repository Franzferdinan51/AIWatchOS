// Hermes link layer — everything ESP-IDF in the Hermes app lives here:
// NVS identity, WiFi STA, the WebSocket to the Hermes hub, the OTA flash
// backend, and the pump task that feeds frames into the protocol client
// (hermes_proto.cpp) via the hermes_core_* entry points in the adapter.
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
#include "hermes_internal.hpp"
#include "hermes_proto.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifdef __ESPRESSIF_IDF__

#include "aiwatchos/hal.hpp"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace aiwatchos_hermes {
namespace {

const char* TAG = "hermes-link";

#ifndef CONFIG_HERMES_WIFI_SSID
#define CONFIG_HERMES_WIFI_SSID ""
#endif
#ifndef CONFIG_HERMES_WIFI_PASSWORD
#define CONFIG_HERMES_WIFI_PASSWORD ""
#endif
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
bool g_started = false;
bool g_have_ip = false;
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

void wifi_event_handler(void*, esp_event_base_t base, int32_t event_id, void* event_data) {
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_have_ip = false;
        esp_wifi_connect();   // WiFi-level retry; WS reconnect is the pump's job
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        g_have_ip = true;
        g_next_attempt_ms = 0;
    }
}

bool wifi_start() {
    if (std::strlen(CONFIG_HERMES_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "no WiFi SSID configured (Kconfig) — Hermes stays offline");
        return false;
    }
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr);
    wifi_config_t wc = {};
    std::snprintf(reinterpret_cast<char*>(wc.sta.ssid), sizeof(wc.sta.ssid), "%s", CONFIG_HERMES_WIFI_SSID);
    std::snprintf(reinterpret_cast<char*>(wc.sta.password), sizeof(wc.sta.password), "%s",
                  CONFIG_HERMES_WIFI_PASSWORD);
    wc.sta.threshold.authmode = std::strlen(CONFIG_HERMES_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    return esp_wifi_start() == ESP_OK;
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

        // (Re)connect management with exponential backoff. After an auth
        // failure the client says wants_reconnect()==false and we stop
        // until reboot — the owner must forget the device on the host.
        int64_t now = now_ms64();
        if (g_have_ip && !g_ws_up && now >= g_next_attempt_ms &&
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
            hermes_core_set_battery(ps.battery_percent <= 100 ? ps.battery_percent : -1);
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
    wifi_start();   // no SSID is a valid, offline-by-config outcome
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
    if (size > g_ota_part->size) return false;   // this tree: slots < protocol max
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
