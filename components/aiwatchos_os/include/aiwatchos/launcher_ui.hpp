// AIWatchOS launcher UI — Muse Gloss tile grid (DESIGN_TOKENS section
// 7, plan 3.1). Each app gets a distinct 96x96 bubble tile with its own
// icon and label (the audit's headline launcher defect was 7 identical
// grey squares). Tiles sit on the shell backdrop — the launcher does
// not paint a background. Tap a tile to launch; the left/right 30px
// edge strips still switch prev/next. Touch coordinates map from
// FT3168 (0..409 x, 0..501 y).
#pragma once

#include <cstdint>
#include "aiwatchos/app.hpp"
#include "aiwatchos/hal.hpp"   // for kDisplayWidth/kDisplayHeight constants

namespace aiwatchos {

class AppManager;   // forward declaration (full definition in app_manager.hpp)

// Tile geometry (tokens section 7): 96px tiles, 3 across — 29px screen
// margin, 28px gutters (+4 centering), rows from y=64 at 148px pitch.
inline constexpr int kTileSize = 96;
inline constexpr int kTileX0 = 33;
inline constexpr int kTileY0 = 64;
inline constexpr int kTilePitchX = 124;
inline constexpr int kTilePitchY = 148;
inline constexpr int kTileCols = 3;

inline void tile_origin(int index, int& x, int& y) {
    x = kTileX0 + (index % kTileCols) * kTilePitchX;
    y = kTileY0 + (index / kTileCols) * kTilePitchY;
}

class LauncherUI {
 public:
    explicit LauncherUI(AppManager& mgr);

    void render(Framebuffer& fb);
    bool on_touch(const TouchEvent& event);

 private:
    AppManager& mgr_;
    int pressed_idx_ = -1;     // tile showing press feedback, if any
    int pressed_frames_ = 0;   // render frames of feedback remaining
};

}  // namespace aiwatchos
