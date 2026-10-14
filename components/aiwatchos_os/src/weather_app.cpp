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
    WeatherSnapshot g_last{};
    bool g_last_seen = false;

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

}  // namespace

// Condition glyph from NWS's own description words (shared by the app
// hero and the clock's weather chip, so they never disagree).
Icon weather_condition_icon(const char* text) {
    if (!text) return Icon::kCloud;
    if (contains_ci(text, "thunder") || contains_ci(text, "rain") ||
        contains_ci(text, "shower") || contains_ci(text, "drizzle")) {
        return Icon::kCloudRain;
    }
    if (contains_ci(text, "snow") || contains_ci(text, "flurr") ||
        contains_ci(text, "blizzard")) {
        return Icon::kCloudSnow;
    }
    if (contains_ci(text, "partly") || contains_ci(text, "mostly")) {
        return Icon::kCloudSun;
    }
    if (contains_ci(text, "sun") || contains_ci(text, "clear") ||
        contains_ci(text, "fair")) {
        return Icon::kSun;
    }
    return Icon::kCloud;
}

namespace {
    // Condition glyph + tint from NWS's own description words.
    void condition_visual(const char* text, Icon* icon, uint16_t* tint) {
        *icon = weather_condition_icon(text);
        switch (*icon) {
            case Icon::kCloudRain: *tint = colors::kBlue; break;
            case Icon::kCloudSnow: *tint = colors::kText; break;
            case Icon::kCloudSun:
            case Icon::kSun:      *tint = colors::kWarn;  break;
            default:              *tint = colors::kDim;   break;
        }
    }
}  // namespace

void weather_set_provider(WeatherSnapshotFn snapshot_fn,
                          WeatherRefreshFn refresh_fn) {
    g_snapshot_fn = snapshot_fn;
    g_refresh_fn = refresh_fn;
}

bool weather_last_snapshot(WeatherSnapshot* out) {
    if (!g_last_seen || !out) return false;
    *out = g_last;
    return true;
}

void WeatherApp::pull_snapshot() {
    if (!g_snapshot_fn) return;
    WeatherSnapshot snap;
    if (g_snapshot_fn(&snap)) {
        snapshot_ = snap;
        provider_seen_ = true;
        g_last = snap;   // R4: the shell's complication feed
        g_last_seen = true;
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
        // Split empty states (§3.7): the icon + title name the regime,
        // the caption names the remedy. No invented numbers, ever.
        Icon icon = Icon::kWifiOff;
        uint16_t icon_tint = colors::kDim;
        const char* title = "OFFLINE";
        const char* why = "JOIN WI-FI TO FETCH";
        if (provider_seen_) {
            switch (snapshot_.status) {
                case WeatherServiceStatus::kConnecting:
                    title = "CONNECTING...";
                    why = "FIRST FETCH IN FLIGHT";
                    break;
                case WeatherServiceStatus::kFetchFailed:
                    title = "FETCH FAILED";
                    why = "TAP TO RETRY";
                    icon_tint = colors::kWarn;
                    break;
                case WeatherServiceStatus::kOk:
                    title = "NO DATA YET";
                    why = "FIRST FETCH IN FLIGHT";
                    break;
                case WeatherServiceStatus::kUnconfigured:
                    icon = Icon::kGear;
                    title = "NOT CONFIGURED";
                    why = "SET A LOCATION IN MDB";
                    break;
            }
        }
        draw_icon(fb, kDisplayWidth / 2 - 20, 176, icon,
                  FontFace::kIcon40, icon_tint);
        draw_text_aa_centered(fb, kDisplayWidth / 2, 240, title,
                              FontFace::kTitle, colors::kText);
        draw_text_aa_centered(fb, kDisplayWidth / 2, 286, why,
                              FontFace::kCaption, colors::kDim);
        // Retry hint only where a tap does something (failed + provider).
        if (provider_seen_ &&
            snapshot_.status == WeatherServiceStatus::kFetchFailed &&
            g_refresh_fn) {
            draw_text_aa_centered(fb, kDisplayWidth / 2, 472, "TAP: RETRY",
                                  FontFace::kMicro, colors::kDim);
        }
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

    // Condition in Title, NWS's own words, wrapped to ≤2 lines with
    // an ellipsis (shared R1 metrics — no byte-chopping UTF-8).
    {
        const auto lines = wrap_text_aa(
            snapshot_.condition_text[0] ? snapshot_.condition_text : "Unknown",
            FontFace::kTitle, 300, 2);
        const int step = font_line_height(FontFace::kTitle) + 2;
        int cy = 262;
        for (const std::string& line : lines) {
            draw_text_aa_centered(fb, kDisplayWidth / 2, cy, line,
                                  FontFace::kTitle, colors::kText);
            cy += step;
        }
    }

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

    // Freshness pill (§3.7): regime dot + UPDATED HH:MM. The dot
    // carries the regime — ok live, warn stale, blue while a refresh
    // is in flight — so the line reads at a glance.
    const bool stale = weather_is_stale(hal().now_ms(),
                                        snapshot_.fetched_epoch_ms,
                                        kStaleAfterMs);
    {
        char pill_text[24];
        std::snprintf(pill_text, sizeof(pill_text), "UPDATED %s", upd_val);
        const int text_w = text_width_aa(pill_text, FontFace::kCaption);
        const int pill_w = 10 + 8 + 6 + text_w + 10;
        const int pill_x = kDisplayWidth / 2 - pill_w / 2;
        const uint16_t dot =
            snapshot_.status == WeatherServiceStatus::kConnecting
                ? colors::kBlue
            : stale ? colors::kWarn
                    : colors::kOk;
        fb.fill_round_rect(pill_x, 426, pill_w, 24, 12, colors::kInput);
        fb.fill_circle(pill_x + 10 + 4, 426 + 12, 4, dot);
        draw_text_aa(fb, pill_x + 10 + 8 + 6, 426 + 4, pill_text,
                     FontFace::kCaption, colors::kText);
    }
    if (g_refresh_fn) {
        draw_text_aa_centered(fb, kDisplayWidth / 2, 472, "TAP: REFRESH",
                              FontFace::kMicro, colors::kDim);
    }
}

bool WeatherApp::on_touch(const TouchEvent& event) {
    if (event.type != TouchEvent::Press || !g_refresh_fn) return false;
    // Tap anywhere refreshes live data; a failed fetch retries from its
    // empty state. Anything else falls through (shell home strip keeps
    // working from the un-failed empty states).
    if (snapshot_.has_data ||
        (provider_seen_ &&
         snapshot_.status == WeatherServiceStatus::kFetchFailed)) {
        g_refresh_fn();
        return true;
    }
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
