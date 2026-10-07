// Battery display — renders a status bar with time and battery % at the top
// of the 410x502 screen. Called each frame by the OS shell before app rendering.
#include "aiwatchos/battery_display.hpp"

namespace aiwatchos {

constexpr uint16_t kStatusBg    = 0x294A;   // dark blue-gray (RGB565)
constexpr uint16_t kTextColor   = 0xFFFF;   // white text
constexpr uint16_t kBatteryFill = 0x7BCF;   // medium gray for battery outline

BatteryDisplay::BatteryDisplay(Hal& board) : board_(board) {}

// 3x5 micro-font for the status-bar clock. Each glyph is 5 rows of 3 bits
// (row 0 = top). Supports '0'-'9' and ':'; anything else renders blank.
namespace {
constexpr int kTimeScale = 4;   // 3x5 cells at 4x -> 12x20 px per glyph, fits the 36px bar

const uint8_t kTimeFont[][5] = {
    {0x7, 0x5, 0x5, 0x5, 0x7},   // '0'
    {0x2, 0x6, 0x2, 0x2, 0x7},   // '1'
    {0x7, 0x1, 0x7, 0x4, 0x7},   // '2'
    {0x7, 0x1, 0x7, 0x1, 0x7},   // '3'
    {0x5, 0x5, 0x7, 0x1, 0x1},   // '4'
    {0x7, 0x4, 0x7, 0x1, 0x7},   // '5'
    {0x7, 0x4, 0x7, 0x5, 0x7},   // '6'
    {0x7, 0x1, 0x2, 0x2, 0x2},   // '7'
    {0x7, 0x5, 0x7, 0x5, 0x7},   // '8'
    {0x7, 0x5, 0x7, 0x1, 0x7},   // '9'
    {0x0, 0x2, 0x0, 0x2, 0x0},   // ':'
};

const uint8_t* time_glyph(char c) {
    if (c >= '0' && c <= '9') return kTimeFont[c - '0'];
    if (c == ':') return kTimeFont[10];
    return nullptr;
}

void draw_time_glyph(Framebuffer& fb, int x, int y, char c, uint16_t color) {
    const uint8_t* rows = time_glyph(c);
    if (!rows) return;
    for (int r = 0; r < 5; ++r) {
        for (int col = 0; col < 3; ++col) {
            if (rows[r] & (0x4 >> col)) {
                fb.fill_rect(x + col * kTimeScale, y + r * kTimeScale,
                             x + col * kTimeScale + kTimeScale - 1,
                             y + r * kTimeScale + kTimeScale - 1, color);
            }
        }
    }
}
}  // namespace

void BatteryDisplay::render(Framebuffer& fb, const char* time_str) {
    PowerStatus ps = board_.read_power();

    // Draw status bar background (top 36 px).
    fb.fill_rect(0, 0, kDisplayWidth - 1, StatusBarConfig{}.height, kStatusBg);

    // Draw battery icon in the top-right corner.
    const int bat_w = 48;
    const int bat_h = 20;
    const int bat_x = kDisplayWidth - bat_w - 8;
    const int bat_y = (StatusBarConfig{}.height - bat_h) / 2;

    // Battery outline.
    fb.hline(bat_x, bat_x + bat_w, bat_y, kBatteryFill);
    fb.hline(bat_x, bat_x + bat_w, bat_y + bat_h - 1, kBatteryFill);
    fb.vline(bat_x, bat_y, bat_y + bat_h - 1, kBatteryFill);
    fb.vline(bat_x + bat_w - 1, bat_y, bat_y + bat_h - 1, kBatteryFill);

    // Battery fill proportional to charge level.
    int fill_w = static_cast<int>((bat_w - 4) * ps.battery_percent / 100u);
    if (fill_w > 0) {
        fb.fill_rect(bat_x + 2, bat_y + 2, bat_x + 2 + fill_w - 1, bat_y + bat_h - 3, kBatteryFill);
    }

    // Charging indicator: fill the battery icon solid when charging.
    if (ps.charging && bat_w > 0) {
        fb.fill_rect(bat_x + 2, bat_y + 2, bat_x + bat_w - 3, bat_y + bat_h - 3, kBatteryFill);
    }

    // Time readout ("H:MM" from app_main) rendered top-left with a 3x5 micro-font.
    if (time_str) {
        int pen_x = 8;
        const int glyph_h = 5 * kTimeScale;
        const int glyph_y = (StatusBarConfig{}.height - glyph_h) / 2;
        for (const char* p = time_str; *p; ++p) {
            draw_time_glyph(fb, pen_x, glyph_y, *p, kTextColor);
            pen_x += 3 * kTimeScale + kTimeScale;   // glyph width + one column spacing
        }
    }
}

}  // namespace aiwatchos
