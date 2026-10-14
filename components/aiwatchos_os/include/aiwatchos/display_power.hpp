// Display power policy for the AIWatchOS main loop.
//
// Pure state machine (no ESP-IDF, no HAL calls) that decides when the panel
// sleeps and wakes. app_main feeds it one sample per frame — current time,
// touch point count, and the button mask from Board::read_buttons() — and
// applies the returned action through Board::display_sleep(). Keeping the
// policy free of hardware calls makes it host-testable like the rest of the
// OS core, and keeps the swallow-the-wake-touch rule in exactly one place.
#pragma once

#include <cstdint>

namespace aiwatchos {

class DisplayPower {
 public:
    enum class Action { kNone, kEnterSleep, kWake };

    // timeout_ms: inactivity period after which the panel sleeps.
    // 0 disables the inactivity timeout (the PWR toggle still works).
    explicit DisplayPower(uint32_t timeout_ms) : timeout_ms_(timeout_ms) {}

    // Advance the policy by one frame.
    //   touch_points: points reported by Board::read_touch() this frame.
    //   buttons: Board::read_buttons() mask (bit 0 = BOOT, bit 1 = PWR).
    Action update(uint64_t now_ms, int touch_points, uint32_t buttons) {
        if (!started_) {
            started_ = true;
            last_activity_ms_ = now_ms;
        }

        const bool touch = touch_points > 0;
        const bool pwr = (buttons & 0x02u) != 0;
        const bool any_input = touch || buttons != 0;
        const bool pwr_down = pwr && !prev_pwr_;
        const bool pwr_up = !pwr && prev_pwr_;
        if (pwr_down) pwr_down_ms_ = now_ms;
        if (any_input) last_activity_ms_ = now_ms;

        Action action = Action::kNone;

        if (asleep_) {
            if (touch) {
                // Wake on touch. The FT3168 keeps running while the panel is
                // in SLPIN, so its INT-driven reads are what produce these
                // points. The waking press is swallowed until every finger
                // lifts, so it cannot also trigger the UI element under it.
                asleep_ = false;
                swallow_until_release_ = true;
                action = Action::kWake;
            } else if (buttons != 0) {
                // Wake on BOOT or PWR press. If PWR caused the wake, the
                // release that follows must not be read as a sleep toggle.
                asleep_ = false;
                if (pwr) pwr_wake_release_pending_ = true;
                action = Action::kWake;
            }
        } else {
            if (swallow_until_release_ && !touch) {
                swallow_until_release_ = false;
            }
            if (pwr_up) {
                if (pwr_wake_release_pending_) {
                    pwr_wake_release_pending_ = false;
                } else if (now_ms - pwr_down_ms_ >= kMinPressMs &&
                           now_ms - pwr_down_ms_ < kLongPressMs) {
                    // Short PWR press toggles the panel off. Long presses are
                    // deliberately not handled at OS level — the release is
                    // ignored here, and the AXP2101 10 s hold-to-off hardware
                    // fallback configured in axp2101_init() stays untouched.
                    asleep_ = true;
                    action = Action::kEnterSleep;
                }
            }
            if (action == Action::kNone && timeout_ms_ > 0 &&
                now_ms - last_activity_ms_ >= timeout_ms_) {
                asleep_ = true;
                action = Action::kEnterSleep;
            }
        }

        prev_pwr_ = pwr;
        return action;
    }

    bool asleep() const { return asleep_; }

    // True while the waking touch (and the rest of that same press, until
    // all fingers lift) must be dropped instead of dispatched to the app.
    bool swallow_touch() const { return swallow_until_release_; }

 private:
    static constexpr uint64_t kMinPressMs = 30;     // debounce floor
    static constexpr uint64_t kLongPressMs = 1000;  // at/above: long press

    uint32_t timeout_ms_;
    bool started_ = false;
    bool asleep_ = false;
    bool swallow_until_release_ = false;
    bool pwr_wake_release_pending_ = false;
    bool prev_pwr_ = false;
    uint64_t last_activity_ms_ = 0;
    uint64_t pwr_down_ms_ = 0;
};

// Idle render pacing (pure, like DisplayPower): while the watch is awake
// but untouched, full render+hash+flush frames are decimated to 1-in-3.
// Input sampling, touch dispatch and app ticks stay at full rate, so the
// first touch still lands within one frame and every input frame renders
// immediately; content that changes without input (streaming text, the
// clock minute, notice TTLs) lands on the next paced frame (~80 ms worst
// case). Cosmetic animation inherits the decimation (the Q6 decoupling,
// applied uniformly). The band-hash flush gate is unaffected: a skipped
// frame leaves the framebuffer untouched, so there is nothing to flush.
class IdleRenderPacer {
 public:
    static constexpr uint32_t kIdleAfterFrames = 125;  // ~5 s at 40 ms
    static constexpr uint32_t kIdleRenderEvery = 3;    // idle renders ~8 fps

    // input: anything happened this frame (touch points/buttons/synth
    // events, MDB injection, an app switch, the frame card). Returns
    // true when this frame must render.
    bool frame(bool input) {
        if (input) {
            idle_ = 0;
            return true;
        }
        ++idle_;
        if (idle_ < kIdleAfterFrames) return true;
        return idle_ % kIdleRenderEvery == 0;
    }

 private:
    uint32_t idle_ = 0;
};

}  // namespace aiwatchos
