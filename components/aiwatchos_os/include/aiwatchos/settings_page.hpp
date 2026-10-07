// AIWatchOS settings page — a touch-driven settings menu for the 410x502 AMOLED.
// Provides controls for brightness, volume, do-not-disturb mode, theme selection,
// and device info (battery status). Designed for hardware constraints: uses simple
// rectangle-based rendering with no dynamic allocation after init().
#pragma once

#include <cstdint>
#include "aiwatchos/app.hpp"
#include "aiwatchos/hal.hpp"

namespace aiwatchos {

struct SettingsState {
    uint8_t brightness = 100;   // 5..100 (matches HAL constraints)
    uint8_t volume = 70;        // 0..100
    bool do_not_disturb = false;
    bool dark_theme = true;     // always on for AMOLED power saving
    int selected_item = 0;      // currently highlighted setting (0-indexed)
};

class SettingsPage {
 public:
    explicit SettingsPage(Hal& board);

    void render(Framebuffer& fb);
    bool on_touch(const TouchEvent& event);

    static App make_app();

 private:
    Hal& board_;
    SettingsState state_{};
};

}  // namespace aiwatchos
