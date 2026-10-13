// Status bar: battery indicator + time display at the top of the screen.
//
// Muse Gloss (DESIGN_TOKENS section 8): the shell owns the top 44px
// ("status zone") exclusively — this renderer repaints the zone's
// backdrop slice first so no app content can show through, then draws
// the section-8 battery cluster (percent text + 25x12 outline + fill)
// and the time in Micro. No background band is painted (the audit's
// "four apps paint a 36px band over the bar" defect class is closed by
// the shell painting this zone LAST every frame).
#pragma once

#include "aiwatchos/app.hpp"
#include "aiwatchos/hal.hpp"

namespace aiwatchos {

// Height of the shell-owned status zone (44 = 6 pad + 32 content row,
// scaled to the 410x502 panel).
inline constexpr int kStatusZoneH = 44;

// The G5 app backdrop: vertical ink -> night -> night-blended-toward-
// blueDeep gradient. The shell paints the full frame before the app
// renders; BatteryDisplay repaints rows [y0, y1) of it. Both use the
// FULL-frame gradient math (vgrad_color over fb.height), so a slice
// always agrees with the whole.
void draw_shell_backdrop(Framebuffer& fb, int y0, int y1);
inline void draw_shell_backdrop(Framebuffer& fb) {
    draw_shell_backdrop(fb, 0, fb.height);
}

// Renders the status bar (time + battery) into the top rows of `fb`.
// Called by the OS shell on every frame, after the foreground app.
class BatteryDisplay {
 public:
    explicit BatteryDisplay(Hal& board) : board_(board) {}

    // Draw the status zone: time from `time_str` ("H:MM"), battery from
    // the board's power read.
    void render(Framebuffer& fb, const char* time_str);

 private:
    Hal& board_;
};

}  // namespace aiwatchos
