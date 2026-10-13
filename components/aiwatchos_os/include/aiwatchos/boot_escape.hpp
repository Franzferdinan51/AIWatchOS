// Shell-level BOOT-hold escape for the AIWatchOS main loop.
//
// Pure state machine (no ESP-IDF, no HAL calls) that decides when a
// BOOT press stops being app input and becomes the shell's "go to
// launcher" gesture. app_main feeds it one sample per frame — current
// time and the BOOT bit from Board::read_buttons() — at the input
// sampling point, ABOVE app dispatch, alongside the DisplayPower
// policy. BOOT doubles as an app key (Muse/Hermes PTT, barge-in) and
// a capture-mode app owns it entirely, so without a shell-level
// escape a dead touch panel traps the user in whatever app is
// foreground. Holding BOOT for kHoldMs fires the escape exactly once
// per hold; app_main then switches to the launcher and masks the BOOT
// bit for the rest of the hold (see latched()), so the press is
// swallowed: the foreground app is gone before it can act on the
// release, and the button cannot re-trigger app or power behaviour
// until it is physically released and pressed again. Short presses
// are untouched and reach apps exactly as before.
#pragma once

#include <cstdint>

namespace aiwatchos {

class BootEscape {
 public:
    // 3000 ms, deliberately ABOVE the in-app BOOT-hold actions:
    // Hermes fires session.new (and, on its AUTH FAIL screen, the
    // key reset) at a 2000 ms hold, accumulated in its foreground
    // tick. The escape is sampled before AppManager::tick each
    // frame, so at an equal 2000 ms threshold it would switch to
    // the launcher on the very frame Hermes fires and starve the
    // action. At 3000 ms an app hold always completes first; the
    // shell escape remains the longer, unmistakable hold.
    static constexpr uint64_t kHoldMs = 3000;

    // Advance the detector by one frame.
    //   boot_down: BOOT bit (button mask bit 0) currently set.
    // Returns true on exactly one frame per hold: the first frame at
    // which the button has been down continuously for >= kHoldMs.
    bool update(bool boot_down, uint64_t now_ms) {
        if (!boot_down) {
            down_ = false;
            latched_ = false;
            return false;
        }
        if (!down_) {
            down_ = true;
            down_since_ms_ = now_ms;
        }
        if (!latched_ && now_ms - down_since_ms_ >= kHoldMs) {
            latched_ = true;
            return true;
        }
        return false;
    }

    // True from the firing frame until the button is released. While
    // latched, app_main masks the BOOT bit out of the frame's button
    // mask, so the remainder of the hold is invisible downstream.
    bool latched() const { return latched_; }

 private:
    bool down_ = false;
    bool latched_ = false;
    uint64_t down_since_ms_ = 0;
};

}  // namespace aiwatchos
