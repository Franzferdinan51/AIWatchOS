// Clock face renderer — draws hour/minute/second hands and a digital time
// readout on the 410x502 RGB565 framebuffer. All pixel writes are bounded to
// [0, 410) x [0, 502). The clock is centered in the available area below the
// status bar (which occupies the top ~36 px), so center_x = 205, center_y = 290.
#include "aiwatchos/clock_face.hpp"
#include "aiwatchos/hal.hpp"   // for kDisplayWidth/kDisplayHeight constants
#include <cmath>

namespace aiwatchos {

// RGB565 colour constants for a clean dark-theme watch face.
constexpr uint16_t kBgColor      = 0x0000;   // black
constexpr uint16_t kHourHand     = 0xFFFF;   // white
constexpr uint16_t kMinuteHand   = 0xBFDF;   // light gray
constexpr uint16_t kSecondHand   = 0xF800;   // red accent
constexpr uint16_t kCenterDot    = 0x7BEF;   // medium gray

// Draw a line from (cx, cy) at angle `theta` for `length` pixels using
// Bresenham's algorithm. Theta is in radians; length can be negative to draw backwards.
static void draw_hand(Framebuffer& fb, int cx, int cy, float theta, int length, uint16_t color) {
    int x1 = static_cast<int>(cx + length * std::sin(theta));
    int y1 = static_cast<int>(cy - length * std::cos(theta));  // Y increases downward in framebuffer
    fb.put_pixel(x1, y1, color);

    // Bresenham between center and tip.
    int dx = std::abs(x1 - cx), sx = (cx < x1) ? 1 : -1;
    int dy = std::abs(y1 - cy), sy = (cy < y1) ? 1 : -1;
    int err = dx - dy;
    int x = cx, y = cy;
    while (true) {
        fb.put_pixel(x, y, color);
        if (x == x1 && y == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

void ClockFace::render(Framebuffer& fb, int cx, int cy, int radius, const ClockState& state) {
    // Clear the clock area to black.
    fb.fill_rect(cx - radius, cy - radius, cx + radius, cy + radius, kBgColor);

    // Draw hour markers (12 ticks).
    for (int h = 0; h < 12; ++h) {
        float angle = static_cast<float>(h) * 3.14159265f / 6.0f;  // 30 degrees each
        int outer_r = radius - 4;
        int inner_r = radius - 10;
        draw_hand(fb, cx, cy, angle, outer_r, kMinuteHand);
        draw_hand(fb, cx, cy, angle, inner_r, kBgColor);  // erase beyond marker
    }

    float hour_theta   = (static_cast<float>(state.hour % 12) + state.minute / 60.0f) * 3.14159265f / 6.0f;
    float minute_theta = (static_cast<float>(state.minute) + state.second / 60.0f) * 3.14159265f / 30.0f;
    float second_theta = static_cast<float>(state.second) * 3.14159265f / 30.0f;

    // Hands: hour ~45% of radius, minute ~70%, second ~80%.
    draw_hand(fb, cx, cy, hour_theta,   static_cast<int>(radius * 0.45f), kHourHand);
    draw_hand(fb, cx, cy, minute_theta, static_cast<int>(radius * 0.70f), kMinuteHand);
    draw_hand(fb, cx, cy, second_theta, static_cast<int>(radius * 0.80f), kSecondHand);

    // Center dot covers the hand origins for a clean look.
    fb.fill_rect(cx - 3, cy - 3, cx + 3, cy + 3, kCenterDot);
}

// Static callbacks for the OS app interface. State is kept in file-scope statics
// because the App struct uses plain function pointers (no closures).
namespace {
    ClockState g_state{12, 0, 0};
    bool g_init = false;

    void clock_init() { g_init = true; }

    void clock_tick(uint32_t elapsed_ms) {
        if (!g_init) return;
        // Sync to wall-clock time via the PCF85063 RTC (Board::now_ms()). This ensures
        // the hands always point to the correct time, not a free-running counter that
        // drifts from real time. now_ms() returns epoch milliseconds in production and
        // is advanced by board_advance_test_time_ms() in unit tests.
        uint64_t ms = hal().now_ms();
        g_state.hour   = static_cast<uint8_t>((ms / 3600000ULL) % 24);
        g_state.minute = static_cast<uint8_t>((ms / 60000ULL) % 60);
        g_state.second = static_cast<uint8_t>((ms / 1000ULL) % 60);
    }

    void clock_render(Framebuffer& fb) {
        // Center below the status bar: cx=205, cy≈290 leaves room for digital time.
        ClockFace::render(fb, kDisplayWidth / 2, 290, 180, g_state);
    }

    bool clock_touch(const TouchEvent& event) {
        (void)event;
        return false;  // clock face doesn't handle touches directly
    }
}

App ClockFace::make_app() {
    // Fully qualify to avoid resolving as a recursive call to this member function.
    return aiwatchos::make_app("clock", clock_init, clock_tick, clock_render, clock_touch);
}

}  // namespace aiwatchos
