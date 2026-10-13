// Weather app — renders current temperature and conditions on the 410x502 AMOLED.
// Layout: large temperature in the center (dominant visual element), condition text
// below, humidity percentage at bottom, plus an "UPDATED HH:MM" line once real
// data exists. The data comes from the weather service (components/
// aiwatchos_weather) through the provider seam in weather_app.hpp; until the
// first real fetch lands, the app renders an honest NO DATA YET state — the
// hard-coded placeholder reading this file used to ship is gone.
#include "aiwatchos/weather_app.hpp"
#include "aiwatchos/colors.hpp"
#include "aiwatchos/font.hpp"
#include <cctype>
#include <cstdio>
#include <cstring>

#ifdef __ESPRESSIF_IDF__
#include "sdkconfig.h"
#endif

namespace aiwatchos {


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

    bool contains_ci(const char* text, const char* needle) {
        const size_t n = std::strlen(needle);
        for (const char* p = text; *p; ++p) {
            size_t i = 0;
            while (i < n && p[i] &&
                   std::tolower(static_cast<unsigned char>(p[i])) ==
                       needle[i]) {
                ++i;
            }
            if (i == n) return true;
        }
        return false;
    }

    // Condition glyph + tint from NWS's own description words.
    void condition_visual(const char* text, Icon* icon, uint16_t* tint) {
        if (contains_ci(text, "thunder") || contains_ci(text, "rain") ||
            contains_ci(text, "shower") || contains_ci(text, "drizzle")) {
            *icon = Icon::kCloudRain;
            *tint = colors::kBlue;
        } else if (contains_ci(text, "snow") || contains_ci(text, "flurr") ||
                   contains_ci(text, "blizzard")) {
            *icon = Icon::kCloudSnow;
            *tint = colors::kText;
        } else if (contains_ci(text, "partly") ||
                   contains_ci(text, "mostly")) {
            *icon = Icon::kCloudSun;
            *tint = colors::kWarn;
        } else if (contains_ci(text, "sun") || contains_ci(text, "clear") ||
                   contains_ci(text, "fair")) {
            *icon = Icon::kSun;
            *tint = colors::kWarn;
        } else {
            *icon = Icon::kCloud;
            *tint = colors::kDim;
        }
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
    // The shell paints backdrop + status zone; content starts below.
    if (!snapshot_.has_data) {
        // Honest empty state — no invented numbers, and the reason why.
        draw_icon(fb, kDisplayWidth / 2 - 20, 176, Icon::kWifiOff,
                  FontFace::kIcon40, colors::kDim);
        draw_text_aa_centered(fb, kDisplayWidth / 2, 240, "NO DATA YET",
                              FontFace::kTitle, colors::kText);
        const char* why = "Offline";
        if (provider_seen_) {
            switch (snapshot_.status) {
                case WeatherServiceStatus::kConnecting:  why = "Connecting";   break;
                case WeatherServiceStatus::kFetchFailed: why = "Fetch failed"; break;
                case WeatherServiceStatus::kOk:          why = "No data yet";  break;
                case WeatherServiceStatus::kUnconfigured: why = "Offline";     break;
            }
        }
        draw_text_aa_centered(fb, kDisplayWidth / 2, 286, why,
                              FontFace::kCaption, colors::kDim);
        return;
    }

    // Condition glyph (64px face, tinted by condition family).
    Icon icon;
    uint16_t tint;
    condition_visual(snapshot_.condition_text, &icon, &tint);
    draw_icon(fb, kDisplayWidth / 2 - 32, 76, icon, FontFace::kIconWx64,
              tint);

    // Hero temperature in Display 96 (`textNum`); the Display face
    // carries a real degree glyph.
    char temp_str[12];
    std::snprintf(temp_str, sizeof(temp_str), "%.0f\xC2\xB0",
                  snapshot_.temperature_f);
    draw_text_aa_centered(fb, kDisplayWidth / 2, 150, temp_str,
                          FontFace::kDisplay, colors::kTextNum);

    // Condition in Title, NWS's own words, pixel-truncated to the row.
    char cond[WeatherSnapshot::kConditionTextCap];
    std::strncpy(cond, snapshot_.condition_text, sizeof(cond) - 1);
    cond[sizeof(cond) - 1] = '\0';
    while (cond[0] &&
           text_width_aa(cond, FontFace::kTitle) > kDisplayWidth - 32) {
        cond[std::strlen(cond) - 1] = '\0';
    }
    draw_text_aa_centered(fb, kDisplayWidth / 2, 262,
                          cond[0] ? cond : "Unknown", FontFace::kTitle,
                          colors::kText);

    // Info cells: the two facts the NWS snapshot actually carries.
    // (Hi/lo, wind and location are NOT in the snapshot — plan 3.6
    // defers them rather than inventing numbers.)
    const uint64_t local = local_display_ms(snapshot_.fetched_epoch_ms);
    char upd_val[8];
    std::snprintf(upd_val, sizeof(upd_val), "%02u:%02u",
                  static_cast<unsigned>((local / 3600000ull) % 24ull),
                  static_cast<unsigned>((local / 60000ull) % 60ull));
    char hum_val[8];
    if (snapshot_.has_humidity) {
        std::snprintf(hum_val, sizeof(hum_val), "%d%%", snapshot_.humidity_pct);
    } else {
        std::snprintf(hum_val, sizeof(hum_val), "--%%");
    }
    struct Cell {
        const char* label;
        const char* value;
    };
    const Cell cells[2] = {{"HUMIDITY", hum_val}, {"UPDATED", upd_val}};
    BubbleStyle cell_style = bubble_style();
    cell_style.shadow = nullptr;
    for (int i = 0; i < 2; ++i) {
        const int cx = 20 + i * (179 + 12);
        const int ccx = cx + 179 / 2;
        fb.draw_bubble(cx, 330, 179, 76, 16, cell_style);
        draw_text_aa_centered(fb, ccx, 342, cells[i].label, FontFace::kMicro,
                              colors::kDim);
        draw_text_aa_centered(fb, ccx, 364, cells[i].value, FontFace::kBody,
                              colors::kText);
    }

    // Freshness line: LIVE, or STALE once the reading is older than
    // one refresh interval.
    const bool stale = weather_is_stale(hal().now_ms(),
                                        snapshot_.fetched_epoch_ms,
                                        kStaleAfterMs);
    draw_text_aa_centered(fb, kDisplayWidth / 2, 428,
                          stale ? "STALE" : "LIVE", FontFace::kCaption,
                          stale ? colors::kWarn : colors::kOk);
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
