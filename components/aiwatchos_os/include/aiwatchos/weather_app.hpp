// AIWatchOS weather app — displays current temperature and conditions on the 410x502 AMOLED.
// Data is REAL: the network-side weather service (components/aiwatchos_weather)
// fetches current conditions from the US National Weather Service
// (api.weather.gov) over HTTPS and publishes snapshots through the provider
// seam below. This app never invents numbers: until the first successful
// fetch lands it renders an honest NO DATA YET state.
// Designed for hardware constraints: simple geometry, no bitmaps, bounded strings.
#pragma once

#include <cstdint>
#include "aiwatchos/app.hpp"
#include "aiwatchos/hal.hpp"   // for kDisplayWidth/kDisplayHeight constants

namespace aiwatchos {

// Staleness rule: a reading fetched at `fetched_epoch_ms` is stale once its
// age exceeds `stale_after_ms` (one full refresh interval — by then a refresh
// should have landed). A fetch timestamp in the future (clock not yet
// disciplined) never counts as stale. Pure; defined in src/weather_data.cpp.
bool weather_is_stale(uint64_t now_epoch_ms, uint64_t fetched_epoch_ms,
                      uint32_t stale_after_ms);

// Link/fetch state reported by the weather service alongside its data, so the
// UI can be honest about WHY there is (or isn't) anything to show.
enum class WeatherServiceStatus : uint8_t {
    kUnconfigured,  // no WiFi credentials configured — nothing will fetch
    kConnecting,    // configured, but no fetch has completed yet
    kOk,            // last fetch succeeded
    kFetchFailed,   // last fetch attempt failed (older data may still exist)
};

// One published weather reading. `has_data` is false until the first
// successful fetch — there is deliberately no default/fake reading.
struct WeatherSnapshot {
    static constexpr size_t kConditionTextCap = 48;
    bool has_data = false;
    float temperature_f = 0.0f;   // Fahrenheit, whole degrees (NWS reports
                                  // deg C; the service converts + rounds)
    int humidity_pct = 0;         // relative humidity, 0..100 — meaningful
                                  // only when has_humidity (NWS stations may
                                  // report an explicit null instead)
    bool has_humidity = false;
    char condition_text[kConditionTextCap] = {};  // NWS textDescription
                                  // verbatim ("Partly Cloudy"); may be empty
    uint64_t fetched_epoch_ms = 0;  // board epoch (RTC/SNTP-disciplined) at fetch time
    WeatherServiceStatus status = WeatherServiceStatus::kUnconfigured;
};

// Provider seam, same pattern as the Muse outbox pump: the network-side
// weather service (components/aiwatchos_weather, firmware builds only)
// registers a snapshot reader + a refresh trigger at startup. With nothing
// registered (host builds, service disabled), the app stays in its honest
// NO DATA YET / OFFLINE state. The snapshot read must be cheap and
// non-blocking (it runs on the UI loop); the refresh trigger only nudges
// the service task, which performs the blocking HTTPS fetch itself.
using WeatherSnapshotFn = bool (*)(WeatherSnapshot* out);
using WeatherRefreshFn = void (*)(void);
void weather_set_provider(WeatherSnapshotFn snapshot_fn,
                          WeatherRefreshFn refresh_fn);

class WeatherApp {
 public:
    void render(Framebuffer& fb);
    bool on_touch(const TouchEvent& event);
    void tick(uint32_t elapsed_ms);
    void on_resume();

    static App make_app();

 private:
    void pull_snapshot();

    WeatherSnapshot snapshot_{};
    bool provider_seen_ = false;  // a snapshot fn answered at least once
};

}  // namespace aiwatchos
