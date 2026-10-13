// Weather fetch service — see weather_service.hpp. The whole implementation
// is firmware-only; on host builds weather_service_start() is a no-op and
// the OS weather app simply never gets a provider (its NO DATA YET state).
#include "aiwatchos_weather/weather_service.hpp"

#include "aiwatchos/weather_app.hpp"   // provider seam + WeatherSnapshot (pulls in hal.hpp)

#ifdef __ESPRESSIF_IDF__

#include <cstdio>
#include <cstring>

#include "sdkconfig.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "aiwatchos/hal.hpp"           // hal().now_ms() — RTC/SNTP-disciplined epoch
#include "aiwatchos_net/net.hpp"       // wifi_connected(), net_config()

namespace aiwatchos_weather {
namespace {

const char* kTag = "aiw-weather";

// NWS GeoJSON bodies: /points ~4 KB, /observations/latest ~5 KB, and the
// station list only fits because the service asks for ?limit=1 (~4 KB;
// unabridged it runs ~75 KB). 16 KB is a hard cap with generous headroom
// — a longer body aborts the fetch instead of growing memory (resource
// discipline: fixed cap, PSRAM, fail closed).
constexpr size_t kMaxBody = 16384;
constexpr uint32_t kTaskPeriodMs = 2000;

// NWS usage policy: every request must carry an identifying User-Agent.
constexpr const char* kUserAgent =
    "AIWatchOS/1.0 (github.com/Franzferdinan51/AIWatchOS)";
constexpr const char* kAccept = "application/geo+json";

aiwatchos::WeatherSnapshot g_snapshot;
SemaphoreHandle_t g_mutex = nullptr;
volatile bool g_refresh_requested = false;
bool g_started = false;

// Resolved latest-observation URL, cached in RAM after the first
// successful chain walk; cleared on any failure so the next cycle
// re-derives the chain (stations do get replaced/renamed).
char g_obs_url[192] = {};

uint32_t refresh_interval_ms() {
#ifdef CONFIG_AIWATCHOS_WEATHER_REFRESH_MIN
    return static_cast<uint32_t>(CONFIG_AIWATCHOS_WEATHER_REFRESH_MIN) * 60000u;
#else
    return 30u * 60000u;
#endif
}

void publish(const aiwatchos::WeatherSnapshot& snap) {
    if (g_mutex) xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_snapshot = snap;
    if (g_mutex) xSemaphoreGive(g_mutex);
}

aiwatchos::WeatherSnapshot current() {
    if (g_mutex) xSemaphoreTake(g_mutex, portMAX_DELAY);
    aiwatchos::WeatherSnapshot snap = g_snapshot;
    if (g_mutex) xSemaphoreGive(g_mutex);
    return snap;
}

bool snapshot_fn(aiwatchos::WeatherSnapshot* out) {
    if (!out) return false;
    *out = current();
    return true;
}

void refresh_fn() { g_refresh_requested = true; }   // nudge only; the task fetches

struct BodySink {
    char* buf = nullptr;
    size_t len = 0;
    size_t cap = 0;
    bool overflow = false;
};

esp_err_t on_http_event(esp_http_client_event_t* evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0)
        return ESP_OK;
    auto* sink = static_cast<BodySink*>(evt->user_data);
    if (!sink || !sink->buf) return ESP_OK;
    if (sink->len + static_cast<size_t>(evt->data_len) > sink->cap) {
        sink->overflow = true;
        return ESP_FAIL;
    }
    std::memcpy(sink->buf + sink->len, evt->data,
                static_cast<size_t>(evt->data_len));
    sink->len += static_cast<size_t>(evt->data_len);
    return ESP_OK;
}

// One blocking GET into the shared capped buffer. True only on HTTP 200
// with the complete body (NUL-terminated into buf).
bool http_get(const char* url, char* body, size_t* out_len) {
    BodySink sink{body, 0, kMaxBody, false};
    esp_http_client_config_t cfg{};
    cfg.url = url;
    cfg.event_handler = &on_http_event;
    cfg.user_data = &sink;
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = true;

    bool ok = false;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_header(client, "User-Agent", kUserAgent);
        esp_http_client_set_header(client, "Accept", kAccept);
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "GET %s failed: %s", url, esp_err_to_name(err));
        } else if (status != 200) {
            ESP_LOGW(kTag, "GET %s -> HTTP %d", url, status);
        } else if (sink.overflow) {
            ESP_LOGW(kTag, "GET %s exceeded %u-byte cap", url,
                     (unsigned)kMaxBody);
        } else {
            body[sink.len] = '\0';
            *out_len = sink.len;
            ok = true;
        }
    }
    return ok;
}

// Walks the NWS chain (points -> station list -> station id) and caches
// the latest-observation URL in g_obs_url. False on any failed hop.
bool resolve_obs_url(char* body) {
    size_t len = 0;
    char points_url[160];
    std::snprintf(points_url, sizeof(points_url),
                  "https://api.weather.gov/points/%s,%s",
                  CONFIG_AIWATCHOS_WEATHER_LAT,
                  CONFIG_AIWATCHOS_WEATHER_LON);
    if (!http_get(points_url, body, &len)) return false;

    char stations_url[256];
    if (!weather_nws_parse_stations_url(body, stations_url,
                                        sizeof(stations_url))) {
        ESP_LOGW(kTag, "points response had no observationStations");
        return false;
    }
    // Only the nearest station is wanted; unabridged lists run ~75 KB.
    std::strncat(stations_url, std::strchr(stations_url, '?') ? "&limit=1"
                                                              : "?limit=1",
                 sizeof(stations_url) - std::strlen(stations_url) - 1);
    if (!http_get(stations_url, body, &len)) return false;

    char station_id[16];
    if (!weather_nws_parse_station_id(body, station_id, sizeof(station_id))) {
        ESP_LOGW(kTag, "station list had no stationIdentifier");
        return false;
    }
    std::snprintf(g_obs_url, sizeof(g_obs_url),
                  "https://api.weather.gov/stations/%s/observations/latest",
                  station_id);
    ESP_LOGI(kTag, "NWS station resolved: %s", station_id);
    return true;
}

// One blocking fetch attempt (chain walk if needed + the observation
// read). Runs ONLY on the weather task.
bool do_fetch() {
    // Response buffer in PSRAM with a hard cap; fail closed if PSRAM is
    // unavailable rather than eating internal DRAM.
    char* body = static_cast<char*>(
        heap_caps_malloc(kMaxBody + 1, MALLOC_CAP_SPIRAM));
    if (!body) {
        ESP_LOGE(kTag, "PSRAM alloc failed — fetch skipped");
        return false;
    }

    bool ok = false;
    size_t len = 0;
    if (g_obs_url[0] == '\0' && !resolve_obs_url(body)) {
        ESP_LOGW(kTag, "NWS chain resolution failed");
    } else if (!http_get(g_obs_url, body, &len)) {
        g_obs_url[0] = '\0';   // stale chain — re-derive next cycle
    } else {
        float temp_f = 0;
        int humidity = 0;
        bool has_humidity = false;
        char desc[aiwatchos::WeatherSnapshot::kConditionTextCap];
        if (weather_nws_parse_observation(body, &temp_f, &humidity,
                                          &has_humidity, desc,
                                          sizeof(desc))) {
            aiwatchos::WeatherSnapshot snap = current();
            snap.has_data = true;
            snap.temperature_f = temp_f;
            snap.humidity_pct = humidity;
            snap.has_humidity = has_humidity;
            std::strncpy(snap.condition_text, desc,
                         sizeof(snap.condition_text) - 1);
            snap.condition_text[sizeof(snap.condition_text) - 1] = '\0';
            snap.fetched_epoch_ms = aiwatchos::hal().now_ms();
            snap.status = aiwatchos::WeatherServiceStatus::kOk;
            publish(snap);
            ESP_LOGI(kTag, "weather: %.0fF, humidity %s, \"%s\"", temp_f,
                     has_humidity ? "reported" : "n/a", desc);
            ok = true;
        } else {
            // Includes the null-temperature case (station lagging): the
            // chain itself may be stale too — re-derive next cycle.
            ESP_LOGW(kTag, "unparseable/lagging observation response");
            g_obs_url[0] = '\0';
        }
    }
    heap_caps_free(body);

    if (!ok) {
        // Keep any last-good reading; only the status degrades.
        aiwatchos::WeatherSnapshot snap = current();
        snap.status = aiwatchos::WeatherServiceStatus::kFetchFailed;
        publish(snap);
    }
    return ok;
}

void weather_task(void* /*arg*/) {
    uint64_t last_attempt_ms = 0;   // esp_timer (monotonic) domain
    bool attempted_once = false;
    for (;;) {
        const bool configured = aiwatchos_net::net_config().wifi_configured();
        aiwatchos::WeatherSnapshot snap = current();
        if (!configured) {
            if (snap.status != aiwatchos::WeatherServiceStatus::kUnconfigured) {
                snap.status = aiwatchos::WeatherServiceStatus::kUnconfigured;
                publish(snap);
            }
        } else {
            if (!snap.has_data &&
                snap.status == aiwatchos::WeatherServiceStatus::kUnconfigured) {
                snap.status = aiwatchos::WeatherServiceStatus::kConnecting;
                publish(snap);
            }
            const uint64_t mono_ms =
                static_cast<uint64_t>(esp_timer_get_time() / 1000);
            const bool due = !attempted_once || g_refresh_requested ||
                             (mono_ms - last_attempt_ms >= refresh_interval_ms());
            if (due && aiwatchos_net::wifi_connected()) {
                attempted_once = true;
                last_attempt_ms = mono_ms;
                g_refresh_requested = false;
                do_fetch();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kTaskPeriodMs));
    }
}

}  // namespace

void weather_service_start() {
    if (g_started) return;
    g_started = true;
#ifndef CONFIG_AIWATCHOS_WEATHER_ENABLE
    ESP_LOGI(kTag, "weather service disabled by Kconfig");
    return;
#endif
    g_mutex = xSemaphoreCreateMutex();
    aiwatchos::weather_set_provider(&snapshot_fn, &refresh_fn);
    // 8 KB stack: the TLS handshake + HTTP client run on this task
    // (mbedTLS with dynamic buffers fits comfortably; the Muse link task's
    // 24 KB was flagged oversized by the resource audit — don't copy it).
    xTaskCreate(&weather_task, "aiw_weather", 8192, nullptr, 3, nullptr);
    ESP_LOGI(kTag, "weather service started (NWS, lat %s, lon %s)",
             CONFIG_AIWATCHOS_WEATHER_LAT, CONFIG_AIWATCHOS_WEATHER_LON);
}

}  // namespace aiwatchos_weather

#else  // host build — no service, the app keeps its honest empty state

namespace aiwatchos_weather {
void weather_service_start() {}
}  // namespace aiwatchos_weather

#endif  // __ESPRESSIF_IDF__
