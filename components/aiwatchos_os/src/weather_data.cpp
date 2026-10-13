// Weather data helpers — pure logic with no HAL/IDF dependencies so the host
// tests can drive the exact shipped code: the WMO code -> label mapping and
// the staleness rule used by the weather app's rendering.
#include "aiwatchos/weather_app.hpp"

namespace aiwatchos {

const char* weather_condition_text(int wmo_code) {
    // WMO weather interpretation codes as used by Open-Meteo.
    switch (wmo_code) {
        case 0:  return "Clear";
        case 1:  return "Mostly Clear";
        case 2:  return "Partly Cloudy";
        case 3:  return "Overcast";
        case 45:
        case 48: return "Fog";
        case 51:
        case 53:
        case 55: return "Drizzle";
        case 56:
        case 57: return "Freezing Drizzle";
        case 61:
        case 63:
        case 65: return "Rain";
        case 66:
        case 67: return "Freezing Rain";
        case 71:
        case 73:
        case 75: return "Snow";
        case 77: return "Snow Grains";
        case 80:
        case 81:
        case 82: return "Rain Showers";
        case 85:
        case 86: return "Snow Showers";
        case 95: return "Thunderstorm";
        case 96:
        case 99: return "Thunderstorm Hail";
        default: return "Unknown";
    }
}

bool weather_is_stale(uint64_t now_epoch_ms, uint64_t fetched_epoch_ms,
                      uint32_t stale_after_ms) {
    if (fetched_epoch_ms == 0) return true;          // never fetched
    if (now_epoch_ms <= fetched_epoch_ms) return false;  // same instant / clock skew
    return (now_epoch_ms - fetched_epoch_ms) >
           static_cast<uint64_t>(stale_after_ms);
}

}  // namespace aiwatchos
