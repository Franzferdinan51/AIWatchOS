// Weather app — renders temperature and conditions on the 410x502 AMOLED.
// Layout: large temperature in the center (dominant visual element), condition text below,
// humidity percentage at bottom. All geometry is hand-computed for the portrait format;
// no dynamic allocation during render() to ensure deterministic frame times.
#include "aiwatchos/weather_app.hpp"
#include <cstdio>
#include <cstring>

namespace aiwatchos {

constexpr uint16_t kBgColor      = 0x0000;   // black (AMOLED native)
constexpr uint16_t kHeaderCol    = 0x294A;   // dark blue header band
constexpr uint16_t kTempColor    = 0xFFFF;   // white large temperature text
constexpr uint16_t kCondColor    = 0xBDEF;   // light gray condition label
constexpr uint16_t kHumidityCol  = 0x7BCF;   // medium gray humidity

void WeatherApp::render(Framebuffer& fb) {
    // Status bar.
    fb.fill_rect(0, 0, kDisplayWidth - 1, 36, kHeaderCol);

    // Center: large temperature display (e.g. "22C") at font scale 4
    // (20x28 px glyphs), centered with the real text width.
    char temp_str[8];
    snprintf(temp_str, sizeof(temp_str), "%dC", data_.temperature_c);
    int temp_x = (kDisplayWidth - Framebuffer::text_width(temp_str, 4)) / 2;
    int temp_y = kDisplayHeight / 2 - 30;
    fb.draw_text(temp_x, temp_y, temp_str, kTempColor, 4);

    // Condition text below temperature.
    const char* cond = data_.condition;
    int cond_x = (kDisplayWidth - Framebuffer::text_width(cond)) / 2;
    fb.fill_rect(0, temp_y + 48, kDisplayWidth - 1, temp_y + 48 + 36, kBgColor);   // clear band
    fb.draw_text(cond_x, temp_y + 48, cond, kCondColor);

    // Humidity at bottom.
    char hum_str[16];
    snprintf(hum_str, sizeof(hum_str), "%d%%", data_.humidity);
    int hum_x = (kDisplayWidth - Framebuffer::text_width(hum_str)) / 2;
    fb.draw_text(hum_x, kDisplayHeight - 80, hum_str, kHumidityCol);
}

bool WeatherApp::on_touch(const TouchEvent& event) {
    (void)event;   // weather is a read-only display in this version
    return false;
}

// Static state for callback-based app interface.
namespace {
    WeatherApp* g_weather = nullptr;

    void weather_init() {}

    void weather_tick(uint32_t elapsed_ms) {
        (void)elapsed_ms;
        // In a real build, this fetches from an HTTP API via WiFi or receives data
        // over BLE from the paired phone. The stub simulates changing conditions.
    }

    void weather_render(Framebuffer& fb) {
        if (g_weather) g_weather->render(fb);
    }

    bool weather_touch(const TouchEvent& event) {
        if (g_weather) return g_weather->on_touch(event);
        return false;
    }
}

App WeatherApp::make_app() {
    static WeatherApp app;
    g_weather = &app;

    // Fully qualify to avoid resolving as a recursive call to this member function.
    return aiwatchos::make_app("weather", weather_init, weather_tick, weather_render, weather_touch);
}

}  // namespace aiwatchos
