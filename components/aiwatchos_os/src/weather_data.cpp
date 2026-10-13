// Weather data helpers — pure logic with no HAL/IDF dependencies so the
// host tests can drive the exact shipped code: the staleness rule used by
// the weather app's rendering. (The WMO code -> label table that used to
// live here died with the Open-Meteo provider; NWS sends human-readable
// textDescription, which the snapshot now carries verbatim.)
#include "aiwatchos/weather_app.hpp"

namespace aiwatchos {

bool weather_is_stale(uint64_t now_epoch_ms, uint64_t fetched_epoch_ms,
                      uint32_t stale_after_ms) {
    if (fetched_epoch_ms == 0) return true;          // never fetched
    if (now_epoch_ms <= fetched_epoch_ms) return false;  // same instant / clock skew
    return (now_epoch_ms - fetched_epoch_ms) >
           static_cast<uint64_t>(stale_after_ms);
}

}  // namespace aiwatchos
