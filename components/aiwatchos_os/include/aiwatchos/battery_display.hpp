// AIWatchOS battery display — renders a status bar with time and battery %
// at the top of the screen, compatible with the 410x502 portrait AMOLED.
#pragma once

#include <cstdint>
#include "aiwatchos/app.hpp"
#include "aiwatchos/hal.hpp"

namespace aiwatchos {

struct StatusBarConfig {
    int height = 36;   // pixels for the top status bar
};

class BatteryDisplay {
 public:
    explicit BatteryDisplay(Hal& board);

    // Render the status bar into the framebuffer (called each frame).
    void render(Framebuffer& fb, const char* time_str);

 private:
    Hal& board_;
};

}  // namespace aiwatchos
