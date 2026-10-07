// AIWatchOS weather app — displays current temperature and conditions on the 410x502 AMOLED.
// Uses a minimal data model (no network stack; fetches via BLE or AI message in production).
// Designed for hardware constraints: simple geometry, no bitmaps, bounded strings.
#pragma once

#include <cstdint>
#include "aiwatchos/app.hpp"
#include "aiwatchos/hal.hpp"   // for kDisplayWidth/kDisplayHeight constants

namespace aiwatchos {

struct WeatherData {
    int8_t temperature_c = 22;   // Celsius (signed: -40..85)
    uint8_t humidity = 50;       // percent 0-100
    const char* condition = "Sunny";   // simple text label
};

class WeatherApp {
 public:
    void render(Framebuffer& fb);
    bool on_touch(const TouchEvent& event);

    static App make_app();

 private:
    WeatherData data_{};
};

}  // namespace aiwatchos
