// Status bar implementation — see battery_display.hpp.
#include "aiwatchos/battery_display.hpp"

#include <cstdio>

#include "aiwatchos/colors.hpp"
#include "aiwatchos/font.hpp"

namespace aiwatchos {

void draw_shell_backdrop(Framebuffer& fb, int y0, int y1) {
    // Bottom stop: night blended 12% toward blueDeep (tokens G5).
    static const uint16_t kBackdropBot =
        lerp565(colors::kNight, colors::kBlueDeep, 31);
    if (y0 < 0) y0 = 0;
    if (y1 > fb.height) y1 = fb.height;
    for (int y = y0; y < y1; y += 4) {
        const uint16_t c =
            vgrad_color(colors::kInk, colors::kNight, kBackdropBot, y,
                        fb.height);
        const int rows = (y + 4 <= y1) ? 4 : (y1 - y);
        for (int i = 0; i < rows; ++i) {
            fb.fill_rect(0, y + i, fb.width - 1, y + i, c);
        }
    }
}

void BatteryDisplay::render(Framebuffer& fb, const char* time_str) {
    // Zone ownership: repaint the backdrop slice first — anything an
    // app drew into y < kStatusZoneBottom is covered before the bar
    // draws. The zone itself starts at kStatusZoneTop (top safe-area
    // inset — the glass masks the first rows; see battery_display.hpp).
    draw_shell_backdrop(fb, 0, kStatusZoneBottom);

    const int text_y = kStatusZoneTop +
        (kStatusZoneH - font_line_height(FontFace::kMicro)) / 2 + 1;

    // Time, left, 20px margin, Micro in `text`.
    if (time_str && time_str[0]) {
        draw_text_aa(fb, 20, text_y, time_str, FontFace::kMicro,
                     colors::kText);
    }

    // Battery cluster, right (tokens section 8): percent text in Micro
    // `dim`, then a 25x12 outline (r3, `dim`) with a 2px nub; the fill
    // is state-coloured: charging = ok, < 20% = danger, else `text`.
    const PowerStatus power = board_.read_power();
    const int pct = power.battery_percent < 0 ? 0
                    : power.battery_percent > 100 ? 100
                                                  : power.battery_percent;
    char pct_str[8];
    std::snprintf(pct_str, sizeof(pct_str), "%d%%", pct);

    constexpr int kOutlineW = 25;
    constexpr int kOutlineH = 12;
    const int right = fb.width - 20;
    const int oy = kStatusZoneTop + (kStatusZoneH - kOutlineH) / 2;
    const int ox = right - 2 - kOutlineW;   // 2px nub to its right

    draw_text_aa_right(fb, ox - 6, text_y, pct_str, FontFace::kMicro,
                       colors::kDim);
    fb.stroke_round_rect(ox, oy, kOutlineW, kOutlineH, 3, colors::kDim,
                         255);
    fb.fill_rect(ox + kOutlineW, oy + 3, ox + kOutlineW + 1, oy + 8,
                 colors::kDim);   // nub, 2 wide x 6 tall

    const int fill_w = 21 * pct / 100;   // inner width is 21px
    if (fill_w > 0) {
        const uint16_t fill_color =
            power.charging ? colors::kOk
            : pct < 20     ? colors::kDanger
                           : colors::kText;
        fb.fill_round_rect(ox + 2, oy + 2, fill_w, kOutlineH - 4, 2,
                           fill_color);
    }
}

}  // namespace aiwatchos
