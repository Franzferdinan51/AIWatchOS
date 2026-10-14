// R4 status bus (ui-overhaul DESIGN §3.1/R4): the shell owns one
// StatusSnapshot and overwrites it every frame with PLAIN VALUES —
// no net/hermes/weather headers leak into the OS component. Apps
// (today: the clock complications) read it through status_snapshot().
// Host tests drive it through status_snapshot_set().
#pragma once

#include <cstdint>
#include "aiwatchos/icons.hpp"

namespace aiwatchos {

// Muse voice-link regime, as plain values (main/ maps LinkState).
enum class MuseChipState : uint8_t { kOff = 0, kIdle = 1, kLive = 2 };
// Hermes hub-link regime (main/ maps HermesLinkState).
enum class HermesChipState : uint8_t { kDown = 0, kPairing = 1, kLink = 2 };

struct StatusSnapshot {
    bool wifi_connected = false;
    bool weather_valid = false;
    float weather_temp_f = 0.0f;
    Icon weather_icon = Icon::kCloud;
    bool weather_stale = false;
    MuseChipState muse = MuseChipState::kOff;
    HermesChipState hermes = HermesChipState::kDown;
    char tz_label[8] = {};   // "UTC-5" — "" hides the clock's TZ tag
};

void status_snapshot_set(const StatusSnapshot& snap);
const StatusSnapshot& status_snapshot();

}  // namespace aiwatchos
