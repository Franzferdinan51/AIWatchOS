// Battery display — renders a status bar with time and battery % at the top
// of the 410x502 screen. Called each frame by the OS shell before app rendering.
#include "aiwatchos/battery_display.hpp"

namespace aiwatchos {

constexpr uint16_t kStatusBg    = 0x294A;   // dark blue-gray (RGB565)
constexpr uint16_t kTextColor   = 0xFFFF;   // white text
constexpr uint16_t kBatteryFill = 0x7BCF;   // medium gray for battery outline

BatteryDisplay::BatteryDisplay(Hal& board) : board_(board) {}

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

    // Charging indicator: show "⚡" text if charging.
    const char* label = ps.charging ? "CHG" : "";
    (void)label;   // placeholder: real impl renders small glyph

    (void)time_str;  // time is rendered as a digital readout in the clock app
}

}  // namespace aiwatchos
