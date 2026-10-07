// Weather app — renders temperature and conditions on the 410x502 AMOLED.
// Layout: large temperature in the center (dominant visual element), condition text below,
// humidity percentage at bottom. All geometry is hand-computed for the portrait format;
// no dynamic allocation during render() to ensure deterministic frame times.
#include "aiwatchos/weather_app.hpp"

namespace aiwatchos {

constexpr uint16_t kBgColor      = 0x0000;   // black (AMOLED native)
constexpr uint16_t kHeaderCol    = 0x294A;   // dark blue header band
constexpr uint16_t kTempColor    = 0xFFFF;   // white large temperature text
constexpr uint16_t kCondColor    = 0xBDEF;   // light gray condition label
constexpr uint16_t kHumidityCol  = 0x7BCF;   // medium gray humidity

void WeatherApp::render(Framebuffer& fb) {
    // Status bar.
    fb.fill_rect(0, 0, kDisplayWidth - 1, 36, kHeaderCol);

    // Center: large temperature display (e.g. "22°").
    char temp_str[8];
    snprintf(temp_str, sizeof(temp_str), "%dC", data_.temperature_c);
    int temp_width = static_cast<int>(strlen(temp_str)) * 14;   // ~14px per digit (large)
    int temp_x = (kDisplayWidth - temp_width) / 2;
    int temp_y = kDisplayHeight / 2 - 30;

    for (size_t i = 0; temp_str[i]; ++i) {
        int cx = temp_x + static_cast<int>(i) * 15;   // double-width characters
        fb.fill_rect(cx, temp_y, cx + 13, temp_y + 27, kTempColor);
    }

    // Condition text below temperature.
    const char* cond = data_.condition;
    int cond_x = (kDisplayWidth - static_cast<int>(strlen(cond)) * 9) / 2;
    fb.fill_rect(0, temp_y + 48, kDisplayWidth - 1, temp_y + 48 + 36, kBgColor);   // clear band

    for (size_t c = 0; cond[c]; ++c) {
        int cx = cond_x + static_cast<int>(c) * 9;
        fb.fill_rect(cx, temp_y + 48, cx + 7, temp_y + 48 + 15, kCondColor);
    }

    // Humidity at bottom.
    char hum_str[16];
    snprintf(hum_str, sizeof(hum_str), "%d%%", data_.humidity);
    int hum_x = (kDisplayWidth - static_cast<int>(strlen(hum_str)) * 9) / 2;

    for (size_t c = 0; hum_str[c]; ++c) {
        int cx = hum_x + static_cast<int>(c) * 9;
        fb.fill_rect(cx, kDisplayHeight - 80, cx + 7, kDisplayHeight - 80 + 15, kHumidityCol);
    }
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
