// AIWatchOS settings page — Muse Gloss rows (DESIGN_TOKENS section 7,
// plan 3.4): bubble rows with an icon chip, a label, and the control
// on the right — REAL sliders for brightness/volume (press + drag,
// forwarded to the HAL on change), a switch for Do Not Disturb, an
// About row with the live battery value, and a Color Test row that
// opens the byte-order pattern (plan 0.1). The old Theme row is gone:
// it was a control that did nothing (audit F2) and honest UI does not
// ship lying controls.
#pragma once

#include <cstdint>
#include "aiwatchos/app.hpp"
#include "aiwatchos/hal.hpp"

namespace aiwatchos {

struct SettingsState {
    uint8_t brightness = 100;   // 5..100 (matches HAL constraints)
    uint8_t volume = 70;        // 0..100
    bool do_not_disturb = false;
};

class SettingsPage {
 public:
    explicit SettingsPage(Hal& board);

    void render(Framebuffer& fb);
    bool on_touch(const TouchEvent& event);

    static App make_app();

 private:
    void set_slider_value(int row, int x);

    Hal& board_;
    SettingsState state_{};
    bool color_test_ = false;   // colour-test pattern is showing
    int drag_row_ = -1;         // slider row being dragged (-1 = none)
};

}  // namespace aiwatchos
