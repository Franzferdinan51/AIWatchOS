// Settings page renderer — touch-driven menu for watch configuration.
// Layout optimized for the 410x502 portrait AMOLED: a status bar at top, a list of
// settings items in the middle (each ~64px tall for finger-friendly tap targets),
// and the selected item's value shown on the right edge. Touch areas are sized
// generously (>= 48dp equivalent) per accessibility guidelines adapted for embedded.
#include "aiwatchos/settings_page.hpp"
#include "aiwatchos/hal.hpp"

namespace aiwatchos {

constexpr uint16_t kBgColor      = 0x0000;   // black (AMOLED native, power saving)
constexpr uint16_t kHeaderColor  = 0x294A;   // dark blue header band
constexpr uint16_t kItemColor    = 0x18E3;   // dark gray item background
constexpr uint16_t kSelectedCol  = 0x52AA;   // blue highlight for selected item
constexpr uint16_t kTextColor    = 0xFFFF;   // white text
constexpr uint16_t kMutedColor   = 0x7BEF;   // medium gray muted/disabled items

// Settings menu items (index matches selected_item).
static const char* kMenuItems[] = {
    "Brightness",
    "Volume",
    "Do Not Disturb",
    "Theme",
    "About Device",
};
constexpr int kNumItems = 5;

SettingsPage::SettingsPage(Hal& board) : board_(board) {}

void SettingsPage::render(Framebuffer& fb) {
    // Status bar at top (36px height).
    fb.fill_rect(0, 0, kDisplayWidth - 1, 36, kHeaderColor);

    // Menu items: each is ~72px tall in the available area below status bar.
    const int item_h = 72;
    const int start_y = 48;   // leave a gap after status bar

    for (int i = 0; i < kNumItems; ++i) {
        int y_top = start_y + i * item_h;
        bool selected = (i == state_.selected_item);

        // Item background — highlight the selected one.
        uint16_t bg = selected ? kSelectedCol : kItemColor;
        fb.fill_rect(8, y_top, kDisplayWidth - 16, y_top + item_h - 8, bg);

        // Draw a simple text label as a row of pixels (placeholder for bitmap font).
        const char* label = kMenuItems[i];
        int label_x = 24;
        int label_y = y_top + item_h / 2 - 8;

        // Render each character as an 8x16 block (simplified — real impl uses font5x7).
        for (size_t c = 0; label[c]; ++c) {
            int cx = label_x + static_cast<int>(c) * 9;
            fb.fill_rect(cx, label_y, cx + 7, label_y + 15, kTextColor);
        }

        // Draw the current value on the right side.
        const char* value_str = nullptr;
        switch (i) {
            case 0:  // Brightness
                if (state_.brightness > 80) value_str = "HIGH";
                else if (state_.brightness < 30) value_str = "LOW";
                else value_str = "MED";
                break;
            case 1:  // Volume
                if (state_.volume > 70) value_str = "ON";
                else if (state_.volume == 0) value_str = "MUTE";
                else value_str = "ON";
                break;
            case 2:  // Do Not Disturb
                value_str = state_.do_not_disturb ? "ON" : "OFF";
                break;
            case 3:  // Theme (always dark for AMOLED)
                value_str = state_.dark_theme ? "DARK" : "LIGHT";
                break;
            case 4:  // About Device — show battery %
                static char batt_buf[16];
                PowerStatus ps = board_.read_power();
                snprintf(batt_buf, sizeof(batt_buf), "%d%%", ps.battery_percent);
                value_str = batt_buf;
                break;
        }

        if (value_str) {
            int val_x = kDisplayWidth - 80;
            for (size_t c = 0; value_str[c]; ++c) {
                int cx = val_x + static_cast<int>(c) * 9;
                fb.fill_rect(cx, label_y, cx + 7, label_y + 15, kMutedColor);
            }
        }
    }

    // Draw scroll indicators (dots on the right edge).
    for (int i = 0; i < kNumItems; ++i) {
        int dot_y = start_y + item_h / 2 - 40 + i * (item_h + 6);
        uint16_t dot_color = (i == state_.selected_item) ? kSelectedCol : kMutedColor;
        fb.fill_rect(kDisplayWidth - 8, dot_y, kDisplayWidth - 4, dot_y + 5, dot_color);
    }
}

bool SettingsPage::on_touch(const TouchEvent& event) {
    const int item_h = 72;
    const int start_y = 48;

    if (event.type == TouchEvent::Press && event.y >= start_y) {
        // Calculate which menu item was tapped.
        int rel_y = event.y - start_y;
        int item_index = rel_y / item_h;

        if (item_index >= 0 && item_index < kNumItems) {
            state_.selected_item = item_index;

            // Toggle/adjust the selected setting immediately on tap.
            switch (item_index) {
                case 0:  // Brightness — cycle through levels
                    if (state_.brightness > 80) state_.brightness = 50;
                    else if (state_.brightness > 30) state_.brightness = 100;
                    else state_.brightness = 80;
                    board_.set_backlight(state_.brightness);
                    break;
                case 1:  // Volume — cycle mute/on
                    state_.volume = (state_.volume == 0) ? 70 : 0;
                    break;
                case 2:  // Do Not Disturb toggle
                    state_.do_not_disturb = !state_.do_not_disturb;
                    break;
                case 3:  // Theme — always dark for AMOLED, but cycle to show UX
                    state_.dark_theme = true;   // fixed: dark theme is mandatory on AMOLED
                    break;
                case 4:  // About Device — no action (read-only info)
                    break;
            }
            return true;   // consumed
        }
    }

    return false;
}

// Static state for the callback-based app interface.
namespace {
    SettingsPage* g_settings = nullptr;

    void settings_init() {}

    void settings_tick(uint32_t) {}

    void settings_render(Framebuffer& fb) {
        if (g_settings) g_settings->render(fb);
    }

    bool settings_touch(const TouchEvent& event) {
        if (g_settings) return g_settings->on_touch(event);
        return false;
    }
}

App SettingsPage::make_app() {
    // Lazily construct the SettingsPage with a reference to the global HAL.
    static Hal& board = hal();
    static SettingsPage settings(board);
    g_settings = &settings;

    // Fully qualify to avoid resolving as a recursive call to this member function.
    return aiwatchos::make_app("settings", settings_init, settings_tick, settings_render, settings_touch);
}

}  // namespace aiwatchos
