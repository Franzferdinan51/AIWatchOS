// Settings page — Muse Gloss rows with real controls. See
// settings_page.hpp for the row inventory.
#include "aiwatchos/settings_page.hpp"
#include "aiwatchos/hal.hpp"
#include "aiwatchos/colors.hpp"
#include "aiwatchos/color_test.hpp"
#include "aiwatchos/font.hpp"
#include <cstdio>

namespace aiwatchos {
namespace {

// Row geometry (tokens section 7): 64px rows from y=56 at 76px pitch.
constexpr int kRowX = 20;
constexpr int kRowW = 370;
constexpr int kRowH = 64;
constexpr int kRowY0 = 56;
constexpr int kRowPitch = 76;

constexpr int kRowBrightness = 0;
constexpr int kRowVolume = 1;
constexpr int kRowDnd = 2;
constexpr int kRowAbout = 3;
constexpr int kRowColorTest = 4;
constexpr int kNumRows = 5;

// Slider track span (the control zone on the right of slider rows).
constexpr int kSliderX0 = 190;
constexpr int kSliderX1 = 330;

int row_top(int row) { return kRowY0 + row * kRowPitch; }
int row_at(int y) {
    if (y < kRowY0) return -1;
    const int rel = y - kRowY0;
    const int row = rel / kRowPitch;
    if (row >= kNumRows || rel % kRowPitch >= kRowH) return -1;
    return row;
}

}  // namespace

SettingsPage::SettingsPage(Hal& board) : board_(board) {}

void SettingsPage::set_slider_value(int row, int x) {
    int pct = (x - kSliderX0) * 100 / (kSliderX1 - kSliderX0);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (row == kRowBrightness) {
        if (pct < 5) pct = 5;   // HAL backlight floor
        state_.brightness = static_cast<uint8_t>(pct);
        board_.set_backlight(state_.brightness);
    } else if (row == kRowVolume) {
        state_.volume = static_cast<uint8_t>(pct);
        board_.set_volume(state_.volume);
    }
}

void SettingsPage::render(Framebuffer& fb) {
    if (color_test_) {
        draw_color_test_pattern(fb);
        return;
    }
    // The shell paints the backdrop + status zone; rows start at y=56.
    struct RowDef {
        const char* label;
        Icon icon;
    };
    static const RowDef kRows[kNumRows] = {
        {"Brightness", Icon::kSun},
        {"Volume", Icon::kVolume},
        {"Do Not Disturb", Icon::kMoon},
        {"About", Icon::kInfo},
        {"Color Test", Icon::kSparkle},
    };

    for (int row = 0; row < kNumRows; ++row) {
        const int y = row_top(row);
        const int cy = y + kRowH / 2;
        BubbleStyle style = bubble_style();
        style.shadow = nullptr;   // rows aren't card-sized; no sprite
        fb.draw_bubble(kRowX, y, kRowW, kRowH, 18, style);

        // Icon chip: 40px pill, blue at 18% over the bubble, glyph in
        // blue; label in Body.
        const uint16_t chip =
            blend565(colors::kBubble, colors::kBlue, 46);
        fb.fill_round_rect(kRowX + 12, cy - 20, 40, 40, 20, chip);
        draw_icon(fb, kRowX + 22, cy - 10, kRows[row].icon,
                  FontFace::kIcon20, colors::kBlueTop);
        draw_text_aa(fb, kRowX + 64,
                     cy - font_line_height(FontFace::kBody) / 2,
                     kRows[row].label, FontFace::kBody, colors::kText);

        char val_buf[16];
        switch (row) {
            case kRowBrightness:
            case kRowVolume: {
                const int value = row == kRowBrightness
                                      ? state_.brightness
                                      : state_.volume;
                // Track + filled portion + % readout.
                fb.fill_round_rect(kSliderX0, cy - 3, kSliderX1 - kSliderX0,
                                   6, 3, colors::kInput);
                const int fill_w = (kSliderX1 - kSliderX0) * value / 100;
                if (fill_w > 0) {
                    fb.fill_round_rect(kSliderX0, cy - 3, fill_w, 6, 3,
                                       colors::kBlue);
                }
                const int thumb_x = kSliderX0 + fill_w;
                const uint16_t thumb =
                    blend565(colors::kBlue, colors::kWhite, 60);
                fb.fill_circle(thumb_x, cy, 14, thumb);
                fb.fill_circle(thumb_x - 3, cy - 4, 5,
                               blend565(thumb, colors::kWhite, 90));
                std::snprintf(val_buf, sizeof(val_buf), "%d%%", value);
                draw_text_aa_right(fb, kRowX + kRowW - 8,
                                   cy - font_line_height(FontFace::kMicro) / 2,
                                   val_buf, FontFace::kMicro, colors::kDim);
                break;
            }
            case kRowDnd: {
                // Switch widget: 44x24 pill + knob.
                const int sx = kRowX + kRowW - 16 - 44;
                fb.fill_round_rect(sx, cy - 12, 44, 24, 12,
                                   state_.do_not_disturb ? colors::kBlue
                                                         : colors::kInput);
                fb.fill_circle(state_.do_not_disturb ? sx + 32 : sx + 12,
                               cy, 9, colors::kWhite);
                break;
            }
            case kRowAbout: {
                const PowerStatus ps = board_.read_power();
                std::snprintf(val_buf, sizeof(val_buf), "Battery %d%%",
                              static_cast<int>(ps.battery_percent));
                draw_text_aa_right(
                    fb, kRowX + kRowW - 16,
                    cy - font_line_height(FontFace::kBody) / 2, val_buf,
                    FontFace::kBody, colors::kDim);
                break;
            }
            case kRowColorTest: {
                draw_icon(fb, kRowX + kRowW - 16 - 20, cy - 10,
                          Icon::kChevronRight, FontFace::kIcon20,
                          colors::kDim);
                break;
            }
        }
    }
}

bool SettingsPage::on_touch(const TouchEvent& event) {
    if (color_test_) {
        // Any press leaves the colour-test pattern (plan 0.1).
        if (event.type == TouchEvent::Press) {
            color_test_ = false;
            return true;
        }
        return false;
    }

    const int row = row_at(event.y);
    switch (event.type) {
        case TouchEvent::Press: {
            if (row < 0 || event.x < kRowX || event.x >= kRowX + kRowW) {
                return false;
            }
            if (row == kRowBrightness || row == kRowVolume) {
                drag_row_ = row;
                if (event.x >= kSliderX0 - 20) {
                    set_slider_value(row, event.x);
                }
                return true;
            }
            if (row == kRowDnd) {
                state_.do_not_disturb = !state_.do_not_disturb;
                return true;
            }
            if (row == kRowColorTest) {
                color_test_ = true;
                return true;
            }
            return row == kRowAbout;   // About: consumed, read-only
        }
        case TouchEvent::Move:
            if (drag_row_ >= 0) {
                set_slider_value(drag_row_, event.x);
                return true;
            }
            return false;
        case TouchEvent::Release:
            if (drag_row_ >= 0) {
                drag_row_ = -1;
                return true;
            }
            return false;
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
