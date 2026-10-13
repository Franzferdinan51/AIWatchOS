// AIWatchOS weather service — real current conditions from Open-Meteo.
//
// The OS weather app (components/aiwatchos_os) renders whatever this service
// publishes through the provider seam (aiwatchos::weather_set_provider). The
// blocking HTTPS fetch runs on this component's own task, never on the UI
// loop: fetch on start, then every AIWATCHOS_WEATHER_REFRESH_MIN minutes
// while WiFi is configured, plus whenever the app nudges a refresh and the
// data is stale. Failures keep the last-good reading and report an honest
// status — nothing here ever fabricates a number.
//
// Open-Meteo needs no API key: GET
//   https://api.open-meteo.com/v1/forecast?latitude=<lat>&longitude=<lon>
//     &current=temperature_2m,relative_humidity_2m,weather_code
//     &temperature_unit=fahrenheit
#pragma once

namespace aiwatchos_weather {

// Extracts the `current` reading from an Open-Meteo forecast response body.
// Pure apart from cJSON: host-testable with the fixture in test/. Returns
// false on malformed JSON, a missing `current` object, or any field that
// is absent or not a number (partial responses are rejected whole — a
// half-reading would be a fabricated one).
bool weather_parse_current(const char* json, float* out_temp_f,
                           int* out_humidity_pct, int* out_weather_code);

// Starts the fetch task and registers the provider with the OS weather
// app. Idempotent. Call once from app_main after aiwatchos_net::net_start().
void weather_service_start();

}  // namespace aiwatchos_weather
