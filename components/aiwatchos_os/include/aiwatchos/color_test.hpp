// Colour-test pattern (plan Phase 0.1) — the on-glass check for the
// CO5300 byte-order fix in display_driver.cpp. Draws the HermesGadget
// reference pattern (pure R/G/B/W/black vertical bands, hg ui.cpp
// m.color_test) plus the 7 Muse state accents and the `bubble` surface
// colour from DESIGN_TOKENS, each labelled with the 5x7 debug font.
// If red and blue trade places on the physical panel, the flush swap is
// wrong; every other UI colour decision is unverifiable until this
// screen reads correctly. Reached from Settings -> Color Test.
#pragma once

#include "aiwatchos/app.hpp"

namespace aiwatchos {

// Draw the full-screen colour-test pattern into fb.
void draw_color_test_pattern(Framebuffer& fb);

}  // namespace aiwatchos
