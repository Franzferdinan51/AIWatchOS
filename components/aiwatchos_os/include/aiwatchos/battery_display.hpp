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

// Glass safe-area frame (stage-4, measured on the real unit with the
// `mdb frame` calibration card; owner's straight-on glass photo,
// 2026-10-13; card borders at insets 0/8/16/24/32 px). The bezel mask
// is small along the straight edges (top ~6 px, bottom ~4 px, sides
// ~4 px: the 8 px line survives along nearly every straight run, the
// 0 px line even peeks through mid-bottom and mid-right), but the
// rounded CORNERS are the real mask: the 24 px corners are eaten
// (worst at the top) and only the 32 px corners are fully clear. The
// owner's original casualties were both corner-anchored: the status
// mini-clock (top-left) and the battery cluster (top-right). Panel
// geometry is NOT a factor: the vendor BSP for this board (waveshare/
// esp32_s3_touch_amoled_2_06 v2.0.0) drives it with y_gap 0 and RASET
// 0..501, byte-identical to our init, so the clip is physical glass.
// Retune HERE only, from a fresh card photo.
inline constexpr int kSafeTop = 30;       // status glyph band starts here
inline constexpr int kSafeBottom = 14;    // no content below H - this
inline constexpr int kSafeLeft = 12;      // no content left of this
inline constexpr int kSafeRight = 12;     // no content right of W - this
// X-inset that corner-anchored content must respect while it sits
// inside the top/bottom bands (the corner curve's reach).
inline constexpr int kSafeCornerEnd = 30;

inline constexpr int kStatusZoneTop = kSafeTop;
inline constexpr int kStatusZoneBottom = kSafeTop + kStatusZoneH;

// The rect apps may paint in: the panel minus the safe insets, below
// the shell-owned status zone. AppManager hands this to apps and
// clips their output to it (AppManager::render), so no app can paint
// into the masked edges.
struct SafeRect { int x0, y0, x1, y1; };   // half-open [x0,x1) x [y0,y1)
inline constexpr SafeRect kContentRect{
    kSafeLeft, kStatusZoneBottom,
    kDisplayWidth - kSafeRight, kDisplayHeight - kSafeBottom};

// Repaint the masked edge bands below the status zone (left/right
// columns + the bottom band) with the shell backdrop, erasing any
// app paint there. Called by AppManager after the app renders; the
// status zone's own rows are repainted by BatteryDisplay::render.
void clip_to_safe_area(Framebuffer& fb);

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
