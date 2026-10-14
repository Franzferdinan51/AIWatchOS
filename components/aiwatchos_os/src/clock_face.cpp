// Clock face — Muse Gloss (DESIGN_TOKENS sections 5-6, plan 3.2).
// The app face is digital-first: the time in Display 96 (textNum), the
// date in Caption `dim`, a state dot row. ClockFace::render() keeps the
// analog face (companion geometry: ringTrack bezel ring, rounded
// cardinal ticks, triangle-weight hands drawn as disc strokes, blue
// second hand) as the alternate face — no red, no square centre dot.
#include "aiwatchos/clock_face.hpp"
#include "aiwatchos/hal.hpp"   // for kDisplayWidth/kDisplayHeight constants
#include "aiwatchos/colors.hpp"
#include "aiwatchos/font.hpp"
#include "aiwatchos/rtc.hpp"
#include "aiwatchos/status_snapshot.hpp"
#include <cmath>
#include <cstdio>

namespace aiwatchos {

// R4 bus storage (lives here so no CMake churn): the shell overwrites
// it every frame; complications read it.
StatusSnapshot g_status_snapshot{};
void status_snapshot_set(const StatusSnapshot& snap) {
    g_status_snapshot = snap;
}
const StatusSnapshot& status_snapshot() { return g_status_snapshot; }

namespace {

// Draw a thick rounded hand as a walk of filled discs from the centre
// toward the tip (angle theta, 0 = twelve o'clock, clockwise).
void draw_hand_discs(Framebuffer& fb, int cx, int cy, float theta,
                     int length, int disc_r, uint16_t color) {
    const float dx = std::sin(theta);
    const float dy = -std::cos(theta);
    for (int t = 0; t <= length; t += 2) {
        fb.fill_circle(static_cast<int>(cx + dx * t),
                       static_cast<int>(cy + dy * t), disc_r, color);
    }
    fb.fill_circle(static_cast<int>(cx + dx * length),
                   static_cast<int>(cy + dy * length), disc_r, color);
}

}  // namespace

void ClockFace::render(Framebuffer& fb, int cx, int cy, int radius, const ClockState& state) {
    // Bezel: full ring in ringTrack, 2px (tokens section 6).
    fb.ring(cx, cy, radius - 2, 2, colors::kRingTrack);

    // Ticks: rounded ticks at 12/3/6/9, dots at the other hours (dim).
    const int r_out = radius - 6;
    for (int h = 0; h < 12; ++h) {
        const float angle = static_cast<float>(h) * 3.14159265f / 6.0f;
        const float sx = std::sin(angle), sy = -std::cos(angle);
        if (h % 3 == 0) {
            for (int t = r_out - 14; t <= r_out; t += 2) {
                fb.fill_circle(static_cast<int>(cx + sx * t),
                               static_cast<int>(cy + sy * t), 2,
                               colors::kDim);
            }
        } else {
            fb.fill_circle(static_cast<int>(cx + sx * (r_out - 7)),
                           static_cast<int>(cy + sy * (r_out - 7)), 2,
                           colors::kDim);
        }
    }

    const float hour_theta   = (static_cast<float>(state.hour % 12) + state.minute / 60.0f) * 3.14159265f / 6.0f;
    const float minute_theta = (static_cast<float>(state.minute) + state.second / 60.0f) * 3.14159265f / 30.0f;
    const float second_theta = static_cast<float>(state.second) * 3.14159265f / 30.0f;

    // Hands in `text`; the second hand is the one blue element.
    draw_hand_discs(fb, cx, cy, hour_theta,   static_cast<int>(radius * 0.45f),
                    radius / 36, colors::kText);
    draw_hand_discs(fb, cx, cy, minute_theta, static_cast<int>(radius * 0.68f),
                    radius / 51, colors::kText);
    draw_hand_discs(fb, cx, cy, second_theta, static_cast<int>(radius * 0.78f),
                    1, colors::kBlue);

    // Centre: blue disc + light core.
    fb.fill_circle(cx, cy, 6, colors::kBlue);
    fb.fill_circle(cx, cy, 3, colors::kBlueTop);
}

// Static callbacks for the OS app interface. State is kept in file-scope statics
// because the App struct uses plain function pointers (no closures).
namespace {
    ClockState g_state{12, 0, 0};
    bool g_init = false;

    // Complication geometry (§3.1): 4 × 78 px chips, 12 px gaps,
    // centred (4*78 + 3*12 = 348; x0 = (410-348)/2).
    constexpr int kChipW = 78;
    constexpr int kChipGap = 12;
    constexpr int kChipX0 = 31;
    constexpr int kChipY = 344;
    constexpr int kChipH = 60;

    ClockNavigateFn g_navigate = nullptr;
    void* g_navigate_ctx = nullptr;

    void clock_init() { g_init = true; }

    void clock_tick(uint32_t elapsed_ms) {
        (void)elapsed_ms;
        if (!g_init) return;
        // Sync to wall-clock time via the PCF85063 RTC (Board::now_ms()).
        // Display derivation uses local time-of-day (configured TZ
        // offset); now_ms() itself stays true UTC epoch.
        uint64_t ms = local_display_ms(hal().now_ms());
        g_state.hour   = static_cast<uint8_t>((ms / 3600000ULL) % 24);
        g_state.minute = static_cast<uint8_t>((ms / 60000ULL) % 60);
        g_state.second = static_cast<uint8_t>((ms / 1000ULL) % 60);
    }

    void clock_render(Framebuffer& fb) {
        // Digital-first face (plan 3.2). The shell has already painted
        // the backdrop and will paint the status zone after us.
        char time_buf[16];
        // 12-hour readout per the design (g_state.hour is 0..23).
        const unsigned hour12 =
            (g_state.hour % 12 == 0) ? 12u : (g_state.hour % 12);
        std::snprintf(time_buf, sizeof(time_buf), "%u:%02u",
                      hour12,
                      static_cast<unsigned>(g_state.minute));
        draw_text_aa_centered(fb, kDisplayWidth / 2, 128, time_buf,
                              FontFace::kDisplay, colors::kTextNum);

        // Date under the time: "Tue, Oct 13" in Caption `dim`.
        const uint64_t ms = local_display_ms(hal().now_ms());
        const RtcTime civil = rtc_civil_from_epoch_ms(ms);
        static const char* kWeekday[7] = {"Sun", "Mon", "Tue", "Wed",
                                          "Thu", "Fri", "Sat"};
        static const char* kMonth[12] = {"Jan", "Feb", "Mar", "Apr",
                                         "May", "Jun", "Jul", "Aug",
                                         "Sep", "Oct", "Nov", "Dec"};
        char date_buf[24];
        std::snprintf(date_buf, sizeof(date_buf), "%s, %s %u",
                      kWeekday[civil.weekday % 7],
                      kMonth[(civil.month - 1) % 12],
                      static_cast<unsigned>(civil.day));
        draw_text_aa_centered(fb, kDisplayWidth / 2, 262, date_buf,
                              FontFace::kCaption, colors::kDim);

        // Dot row: battery state + charging (link state lives in the
        // shell's status strip; the OS component has no net access).
        const PowerStatus power = hal().read_power();
        const uint16_t dot_batt =
            power.battery_percent < 20 ? colors::kDanger : colors::kFaint;
        const uint16_t dot_charge =
            power.charging ? colors::kBlue : colors::kFaint;
        fb.fill_circle(kDisplayWidth / 2 - 14, 322, 4, dot_batt);
        fb.fill_circle(kDisplayWidth / 2 + 14, 322, 4, dot_charge);
        // TZ tag next to the dots (the shell fills the label; the OS
        // never touches sdkconfig/Kconfig).
        const StatusSnapshot& snap = status_snapshot();
        if (snap.tz_label[0]) {
            draw_text_aa(fb, kDisplayWidth / 2 + 30, 314, snap.tz_label,
                         FontFace::kMicro, colors::kDim);
        }

        // Complications (§3.1): four 78px chips fed by the R4 snapshot.
        // Each chip is a button into its app (wifi→Settings,
        // weather→Weather, mic→Muse, feather→Hermes).
        struct Chip {
            Icon icon;
            uint16_t tint;
            char label[8];
            const char* target;
        };
        Chip chips[4];
        chips[0] = {snap.wifi_connected ? Icon::kWifi : Icon::kWifiOff,
                    snap.wifi_connected ? colors::kText : colors::kDim,
                    {0}, "settings"};
        {
            const char* s = snap.wifi_connected ? "ON" : "OFF";
            chips[0].label[0] = s[0];
            chips[0].label[1] = s[1];
            chips[0].label[2] = s[2];
        }
        chips[1] = {snap.weather_icon,
                    (snap.weather_valid && !snap.weather_stale)
                        ? colors::kText
                        : colors::kDim,
                    {0}, "weather"};
        if (snap.weather_valid) {
            std::snprintf(chips[1].label, sizeof(chips[1].label), "%.0f\xC2\xB0",
                          static_cast<double>(snap.weather_temp_f));
        } else {
            chips[1].label[0] = '-';
            chips[1].label[1] = '-';
        }
        {
            const char* s = "OFF";
            uint16_t tint = colors::kDim;
            if (snap.muse == MuseChipState::kLive) {
                s = "LIVE";
                tint = colors::kOk;
            } else if (snap.muse == MuseChipState::kIdle) {
                s = "IDLE";
                tint = colors::kWarn;
            }
            chips[2] = {Icon::kMic, tint, {0}, "muse"};
            chips[2].label[0] = s[0];
            chips[2].label[1] = s[1];
            if (s[2]) chips[2].label[2] = s[2];
            if (s[3]) chips[2].label[3] = s[3];
        }
        {
            const char* s = "DOWN";
            uint16_t tint = colors::kDim;
            if (snap.hermes == HermesChipState::kLink) {
                s = "LINK";
                tint = colors::kOk;
            } else if (snap.hermes == HermesChipState::kPairing) {
                s = "PAIR";
                tint = colors::kWarn;
            }
            chips[3] = {Icon::kFeather, tint, {0}, "hermes"};
            for (int c = 0; c < 4 && s[c]; ++c) chips[3].label[c] = s[c];
        }
        for (int i = 0; i < 4; ++i) {
            const int cx = kChipX0 + i * (kChipW + kChipGap);
            draw_icon(fb, cx + (kChipW - 20) / 2, kChipY + 4, chips[i].icon,
                      FontFace::kIcon20, chips[i].tint);
            draw_text_aa_centered(fb, cx + kChipW / 2, kChipY + 30,
                                  chips[i].label, FontFace::kCaption,
                                  colors::kText);
        }
    }

    bool clock_touch(const TouchEvent& event) {
        if (event.type != TouchEvent::Press || !g_navigate) return false;
        if (event.y < kChipY || event.y >= kChipY + kChipH) return false;
        for (int i = 0; i < 4; ++i) {
            const int cx = kChipX0 + i * (kChipW + kChipGap);
            if (event.x >= cx && event.x < cx + kChipW) {
                static const char* kTargets[4] = {"settings", "weather",
                                                  "muse", "hermes"};
                g_navigate(kTargets[i], g_navigate_ctx);
                return true;
            }
        }
        return false;
    }
}

void clock_set_navigate_handler(ClockNavigateFn fn, void* ctx) {
    g_navigate = fn;
    g_navigate_ctx = ctx;
}

App ClockFace::make_app() {
    // Fully qualify to avoid resolving as a recursive call to this member function.
    return aiwatchos::make_app("clock", clock_init, clock_tick, clock_render, clock_touch);
}

}  // namespace aiwatchos
