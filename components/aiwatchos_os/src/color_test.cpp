// Colour-test pattern — see color_test.hpp for why this screen exists.
#include "aiwatchos/color_test.hpp"

namespace aiwatchos {

void draw_color_test_pattern(Framebuffer& fb) {
    const int w = fb.width;
    const int h = fb.height;

    // Bands (HermesGadget m.color_test): five vertical bands of pure
    // red, green, blue, white, black over the top ~60% of the panel.
    // Canonical 565 primaries: R = 0xF800, G = 0x07E0, B = 0x001F.
    const uint16_t bands[5] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000};
    const char* band_names[5] = {"RED", "GRN", "BLU", "WHT", "BLK"};
    const int band_bottom = h * 3 / 5;
    for (int i = 0; i < 5; ++i) {
        const int left = w * i / 5;
        const int right = w * (i + 1) / 5;
        fb.fill_rect(left, 0, right - 1, band_bottom - 1, bands[i]);
        // Label in the contrasting extreme so it stays readable.
        const uint16_t label_color = (i == 3 || i == 1) ? 0x0000 : 0xFFFF;
        fb.draw_text(left + 8, 12, band_names[i], label_color, 2);
    }

    // The 7 Muse state accents as swatches (DESIGN_TOKENS section 3).
    const int sw_y = band_bottom + 16;
    const int sw_h = 56;
    const int sw_w = (w - 32) / 7;
    for (int i = 0; i < 7; ++i) {
        const MuseState st = static_cast<MuseState>(i);
        fb.fill_rect(16 + i * sw_w, sw_y, 16 + (i + 1) * sw_w - 5,
                     sw_y + sw_h - 1, state_scheme(st).accent);
    }
    fb.draw_text(16, sw_y + sw_h + 8, "STATE ACCENTS", 0xFFFF, 2);

    // The bubble surface colour (the most-used surface in the UI) and
    // the primary chrome blue, side by side — both are blue-family but
    // must stay distinguishable and neither may lean green/red.
    const int bb_y = sw_y + sw_h + 40;
    fb.fill_rect(16, bb_y, w / 2 - 8, bb_y + 55, colors::kBubble);
    fb.fill_rect(w / 2 + 8, bb_y, w - 17, bb_y + 55, colors::kBlue);
    fb.draw_text(16, bb_y + 64, "BUBBLE 14305A", 0xFFFF, 2);
    fb.draw_text(w / 2 + 8, bb_y + 64, "BLUE 1877F2", 0xFFFF, 2);
}

}  // namespace aiwatchos
