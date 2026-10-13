// WiFi STA bring-up + SNTP discipline for the board clock.
//
// Adapted to ESP-IDF 5.3 (the muse-gadget-206 reference targets 5.5+/6.x;
// everything used here — esp_wifi event API, esp_sntp, esp_netif — is the
// 5.3 spelling of the same design: station mode, reconnect with capped
// backoff, SNTP poll mode started on first GOT_IP).
//
// SNTP -> clock: the net task watches the system time; once it is sane
// (post-2024) it calls aiwatchos::board_set_epoch_ms() — the additive seam
// in aiwatchos_os — so Board::now_ms() shows real wall-clock time. The seam
// shifts the existing base+uptime math; the base constant is untouched
// (a build-time fallback lives on another branch).
#include "aiwatchos_net/net.hpp"

#include <atomic>
#include <cstring>
#include <ctime>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "aiwatchos/hal.hpp"

namespace aiwatchos_net {
namespace {

const char* kTag = "aiw-net";

std::atomic<WifiState> g_wifi{WifiState::kOff};
std::atomic<bool> g_time_synced{false};
std::atomic<int> g_retry{0};
bool g_sntp_started = false;

void on_wifi_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi.store(WifiState::kConnecting);
        int n = g_retry.fetch_add(1) + 1;
        int delay_s = n > 5 ? 30 : (1 << (n - 1));  // 1,2,4,8,16,30 cap
        ESP_LOGW(kTag, "wifi disconnected; retry %d in %ds", n, delay_s);
        vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(kTag, "wifi got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        g_retry.store(0);
        g_wifi.store(WifiState::kConnected);
        if (!g_sntp_started) {
            g_sntp_started = true;
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, net_config().sntp_server.c_str());
            esp_sntp_init();
            ESP_LOGI(kTag, "sntp started (%s)", net_config().sntp_server.c_str());
        }
    }
}

// Polls the system clock; once SNTP makes it sane, discipline the board
// clock through the seam, then re-check hourly (SNTP keeps the system
// clock right; the board base only needs the one shift, refreshed to
// absorb drift between the two time bases).
void time_task(void*) {
    bool applied = false;
    TickType_t last_apply = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        std::time_t now = std::time(nullptr);
        if (now < 1704067200) continue;  // before 2024-01-01: not synced
        TickType_t ticks = xTaskGetTickCount();
        if (!applied || ticks - last_apply > pdMS_TO_TICKS(3600 * 1000)) {
            uint64_t epoch_ms = static_cast<uint64_t>(now) * 1000;
            aiwatchos::board_set_epoch_ms(epoch_ms);
            g_time_synced.store(true);
            applied = true;
            last_apply = ticks;
            ESP_LOGI(kTag, "clock disciplined from SNTP (epoch %llu)",
                     static_cast<unsigned long long>(epoch_ms));
        }
    }
}

bool g_started = false;

}  // namespace

WifiState wifi_state() { return g_wifi.load(); }

const char* wifi_state_name(WifiState s) {
    switch (s) {
        case WifiState::kOff: return "OFF";
        case WifiState::kNoCredentials: return "NO-CREDS";
        case WifiState::kConnecting: return "CONNECTING";
        case WifiState::kConnected: return "CONNECTED";
        case WifiState::kFailed: return "FAILED";
    }
    return "?";
}

bool wifi_connected() { return g_wifi.load() == WifiState::kConnected; }
bool net_time_synced() { return g_time_synced.load(); }

// Defined in muse_link.cpp.
void muse_link_task_start();
// Defined in muse_tts.cpp.
void muse_tts_start();

void net_start() {
    if (g_started) return;
    g_started = true;

#if !CONFIG_AIWATCHOS_NET_ENABLE
    ESP_LOGI(kTag, "net component disabled by Kconfig");
    return;
#else
    // NVS first: the config layer merges NVS over Kconfig, and nothing
    // else in AIWatchOS initializes NVS today. net_config() caches on
    // first read, so this must happen before the first net_config() call.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_flash_init failed: %s", esp_err_to_name(err));
        g_wifi.store(WifiState::kFailed);
        return;
    }

    const NetConfig& cfg = net_config();
    if (!cfg.wifi_configured()) {
        // Unconfigured = the pre-network watch, unchanged: outbox mode.
        ESP_LOGI(kTag, "no wifi credentials configured; staying offline");
        g_wifi.store(WifiState::kNoCredentials);
        return;
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) {
        g_wifi.store(WifiState::kFailed);
        return;
    }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &on_wifi_event, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &on_wifi_event, nullptr, nullptr);

    wifi_config_t wc{};
    std::strncpy(reinterpret_cast<char*>(wc.sta.ssid), cfg.wifi_ssid.c_str(),
                 sizeof(wc.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wc.sta.password), cfg.wifi_pass.c_str(),
                 sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = cfg.wifi_pass.empty() ? WIFI_AUTH_OPEN
                                                      : WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    g_wifi.store(WifiState::kConnecting);
    if (esp_wifi_start() != ESP_OK) {
        g_wifi.store(WifiState::kFailed);
        return;
    }
    ESP_LOGI(kTag, "wifi sta starting for ssid '%s'", cfg.wifi_ssid.c_str());

    xTaskCreate(time_task, "aiw_time", 3072, nullptr, 4, nullptr);
    muse_link_task_start();
    muse_tts_start();
#endif
}

}  // namespace aiwatchos_net
