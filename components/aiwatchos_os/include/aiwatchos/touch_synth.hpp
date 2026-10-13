// Touch event synthesis (audit X6 / plan 1.7). The FT3168 reports only
// "points currently down" per poll — it has no Press/Release/Move event
// semantics. app_main used to synthesize Press alone, every frame, which
// meant Muse's push-to-talk could start but never end by touch and
// drag-scroll was dead code. This state machine turns the point stream
// into a proper event sequence:
//   point appears          -> Press at its position
//   point moves >= 2 px    -> Move (new position)
//   point disappears       -> Release at its last position
// Points are tracked by slot index; the FT3168 reports a stable order
// for the single-finger interactions this UI uses (a two-finger slot
// swap degrades to a Move, which no current app can observe as wrong).
#pragma once

#include "aiwatchos/app.hpp"

namespace aiwatchos {

class TouchSynth {
 public:
    static constexpr int kMaxPoints = 2;
    static constexpr int kMaxEvents = 4;   // 2 points x (press+move) worst case

    // Feed one frame's points; writes up to kMaxEvents events into
    // out_events and returns the count (0 = nothing changed).
    int update(int n_points, const int* xs, const int* ys,
               TouchEvent* out_events);

    // Forget all points (e.g. across sleep, so a finger held through a
    // wake cycle cannot produce a phantom Release).
    void reset();

 private:
    bool down_[kMaxPoints] = {};
    int x_[kMaxPoints] = {};
    int y_[kMaxPoints] = {};
};

}  // namespace aiwatchos
