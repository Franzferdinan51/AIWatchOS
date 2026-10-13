// Weather app — renders current temperature and conditions on the 410x502 AMOLED.
// Layout: large temperature in the center (dominant visual element), condition text
// below, humidity percentage at bottom, plus an "UPDATED HH:MM" line once real
// data exists. The data comes from the weather service (components/
// aiwatchos_weather) through the provider seam in weather_app.hpp; until the
// first real fetch lands, the app renders an honest NO DATA YET state — the
// hard-coded placeholder reading this file used to ship is gone.
#include "aiwatchos/weather_app.hpp"
#include <cstdio>
#include <cstring>

#ifdef __ESPRESSIF_IDF__
#include "sdkconfig.h"
#endif

namespace aiwatchos {

constexpr uint16_t kBgColor      = 0x0000;   // black (AMOLED native)
constexpr uint16_t kHeaderCol    = 0x294A;   // dark blue header band
constexpr uint16_t kTempColor    = 0xFFFF;   // white large temperature text
constexpr uint16_t kCondColor    = 0xBDEF;   // light gray condition label
constexpr uint16_t kHumidityCol  = 0x7BCF;   // medium gray humidity
constexpr uint16_t kDimColor     = 0x7BCF;   // medium gray status/updated lines

// A reading is stale once it is older than one refresh interval (by then the
// service should have refreshed it). The interval mirrors the weather
// component's Kconfig; host builds use its default.
#if defined(__ESPRESSIF_IDF__) && defined(CONFIG_AIWATCHOS_WEATHER_REFRESH_MIN)
constexpr uint32_t kStaleAfterMs =
    static_cast<uint32_t>(CONFIG_AIWATCHOS_WEATHER_REFRESH_MIN) * 60000u;
#else
constexpr uint32_t kStaleAfterMs = 30u * 60000u;
#endif

namespace {
    WeatherSnapshotFn g_snapshot_fn = nullptr;
    WeatherRefreshFn g_refresh_fn = nullptr;

    void draw_centered(Framebuffer& fb, int y, const char* text,
                       uint16_t color, int scale = 2) {
        int x = (kDisplayWidth - Framebuffer::text_width(text, scale)) / 2;
        fb.draw_text(x, y, text, color, scale);
    }
}  // namespace

void weather_set_provider(WeatherSnapshotFn snapshot_fn,
                          WeatherRefreshFn refresh_fn) {
    g_snapshot_fn = snapshot_fn;
    g_refresh_fn = refresh_fn;
}

void WeatherApp::pull_snapshot() {
    if (!g_snapshot_fn) return;
    WeatherSnapshot snap;
    if (g_snapshot_fn(&snap)) {
        snapshot_ = snap;
        provider_seen_ = true;
    }
}

void WeatherApp::tick(uint32_t elapsed_ms) {
    (void)elapsed_ms;
    // Cheap snapshot pull (mutex-guarded struct copy on the service side);
    // the blocking HTTPS fetch never happens here — it lives on the
    // weather service task.
    pull_snapshot();
}

void WeatherApp::on_resume() {
    pull_snapshot();
    // Fetch on open when there's nothing fresh to show.
    if (!g_refresh_fn) return;
    if (!snapshot_.has_data ||
        weather_is_stale(hal().now_ms(), snapshot_.fetched_epoch_ms,
                         kStaleAfterMs)) {
        g_refresh_fn();
    }
}

void WeatherApp::render(Framebuffer& fb) {
    // Status bar.
    fb.fill_rect(0, 0, kDisplayWidth - 1, 36, kHeaderCol);

    const int center_y = kDisplayHeight / 2 - 30;

    if (!snapshot_.has_data) {
        // Honest empty state — no invented numbers, and the reason why.
        draw_centered(fb, center_y, "NO DATA YET", kTempColor, 3);
        const char* why = "OFFLINE";
        if (provider_seen_) {
            switch (snapshot_.status) {
                case WeatherServiceStatus::kConnecting:  why = "CONNECTING";  break;
                case WeatherServiceStatus::kFetchFailed: why = "FETCH FAILED"; break;
                case WeatherServiceStatus::kOk:          why = "NO DATA YET";  break;
                case WeatherServiceStatus::kUnconfigured: why = "OFFLINE";    break;
            }
        }
        draw_centered(fb, center_y + 64, why, kCondColor, 2);
        return;
    }

    // Center: large temperature display (e.g. "74F") at font scale 4
    // (20x28 px glyphs), centered with the real text width. Fahrenheit;
    // the 5x7 charset has no degree glyph, so a plain F follows the value.
    char temp_str[12];
    snprintf(temp_str, sizeof(temp_str), "%.0fF", snapshot_.temperature_f);
    int temp_x = (kDisplayWidth - Framebuffer::text_width(temp_str, 4)) / 2;
    fb.draw_text(temp_x, center_y, temp_str, kTempColor, 4);

    // Condition text below temperature, derived from the WMO weather code.
    const char* cond = weather_condition_text(snapshot_.weather_code);
    fb.fill_rect(0, center_y + 48, kDisplayWidth - 1, center_y + 48 + 36, kBgColor);   // clear band
    draw_centered(fb, center_y + 48, cond, kCondColor, 2);

    // "UPDATED HH:MM" (local display time) + STALE tag when the reading is
    // older than one refresh interval.
    const uint64_t now = hal().now_ms();
    const uint64_t local = local_display_ms(snapshot_.fetched_epoch_ms);
    const unsigned hh = static_cast<unsigned>((local / 3600000ull) % 24ull);
    const unsigned mm = static_cast<unsigned>((local / 60000ull) % 60ull);
    char upd_str[24];
    if (weather_is_stale(now, snapshot_.fetched_epoch_ms, kStaleAfterMs)) {
        snprintf(upd_str, sizeof(upd_str), "UPDATED %02u:%02u STALE", hh, mm);
    } else {
        snprintf(upd_str, sizeof(upd_str), "UPDATED %02u:%02u", hh, mm);
    }
    draw_centered(fb, kDisplayHeight - 116, upd_str, kDimColor, 2);

    // Humidity at bottom.
    char hum_str[16];
    snprintf(hum_str, sizeof(hum_str), "%d%%", snapshot_.humidity_pct);
    int hum_x = (kDisplayWidth - Framebuffer::text_width(hum_str)) / 2;
    fb.draw_text(hum_x, kDisplayHeight - 80, hum_str, kHumidityCol);
}

bool WeatherApp::on_touch(const TouchEvent& event) {
    (void)event;   // weather is a read-only display in this version
    return false;
}

// Static state for callback-based app interface.
namespace {
    WeatherApp* g_weather = nullptr;

    void weather_init() {}

    void weather_tick(uint32_t elapsed_ms) {
        if (g_weather) g_weather->tick(elapsed_ms);
    }

    void weather_render(Framebuffer& fb) {
        if (g_weather) g_weather->render(fb);
    }

    bool weather_touch(const TouchEvent& event) {
        if (g_weather) return g_weather->on_touch(event);
        return false;
    }

    void weather_resume() {
        if (g_weather) g_weather->on_resume();
    }
}

App WeatherApp::make_app() {
    static WeatherApp app;
    g_weather = &app;

    // Fully qualify to avoid resolving as a recursive call to this member function.
    return aiwatchos::make_app("weather", weather_init, weather_tick, weather_render,
                               weather_touch, nullptr, weather_resume);
}

}  // namespace aiwatchos
