// AIWatchOS clock face — renders hour/minute/second hands and a digital time
// display on the 410x502 RGB565 framebuffer. Designed for portrait orientation.
#pragma once

#include <cstdint>
#include "aiwatchos/app.hpp"

namespace aiwatchos {

struct ClockState {
    uint8_t hour;     // 0..23
    uint8_t minute;   // 0..59
    uint8_t second;   // 0..59
};

class ClockFace {
 public:
    // Render the clock face centered in an area of (cx, cy) with given radius.
    static void render(Framebuffer& fb, int cx, int cy, int radius, const ClockState& state);

    // Build a complete app descriptor for the clock face. The OS registers this
    // as App ID "clock". Uses no dynamic memory after init().
    static App make_app();
};

}  // namespace aiwatchos
