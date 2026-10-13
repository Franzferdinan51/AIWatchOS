// Touch event synthesis — see touch_synth.hpp.
#include "aiwatchos/touch_synth.hpp"

#include <cstdlib>

namespace aiwatchos {

void TouchSynth::reset() {
    for (int i = 0; i < kMaxPoints; ++i) down_[i] = false;
}

int TouchSynth::update(int n_points, const int* xs, const int* ys,
                       TouchEvent* out_events) {
    if (n_points < 0) n_points = 0;
    if (n_points > kMaxPoints) n_points = kMaxPoints;
    int n_events = 0;
    auto emit = [&](TouchEvent::Type type, int x, int y) {
        if (n_events < kMaxEvents) {
            out_events[n_events++] = TouchEvent{x, y, type};
        }
    };
    for (int i = 0; i < kMaxPoints; ++i) {
        const bool now_down = i < n_points;
        if (now_down && !down_[i]) {
            emit(TouchEvent::Press, xs[i], ys[i]);
            down_[i] = true;
            x_[i] = xs[i];
            y_[i] = ys[i];
        } else if (now_down && down_[i]) {
            if (std::abs(xs[i] - x_[i]) >= 2 || std::abs(ys[i] - y_[i]) >= 2) {
                emit(TouchEvent::Move, xs[i], ys[i]);
                x_[i] = xs[i];
                y_[i] = ys[i];
            }
        } else if (!now_down && down_[i]) {
            emit(TouchEvent::Release, x_[i], y_[i]);
            down_[i] = false;
        }
    }
    return n_events;
}

}  // namespace aiwatchos
