// AIWatchOS launcher UI — a touch-driven app grid with swipe navigation.
// Renders on the 410x502 portrait AMOLED, supports tap-to-launch and edge-swipe
// to switch between apps. Touch coordinates mapped from FT3168 (0..409 x, 0..501 y).
#pragma once

#include <cstdint>
#include "aiwatchos/app.hpp"

namespace aiwatchos {

class AppManager;

struct LauncherConfig {
    int icon_size = 64;   // pixels per app icon (square)
    int cols = 3;         // grid columns across the 410px width
    int rows = 5;         // grid rows down the 502px height (leaves room for status bar)
};

class LauncherUI {
 public:
    explicit LauncherUI(AppManager& mgr);

    void render(Framebuffer& fb);
    bool on_touch(const TouchEvent& event);

 private:
    AppManager& mgr_;
    int scroll_offset_ = 0;   // vertical pixels scrolled in the app grid
};

}  // namespace aiwatchos
