// AIWatchOS weather service — real current conditions from the US
// National Weather Service (api.weather.gov).
//
// The OS weather app (components/aiwatchos_os) renders whatever this
// service publishes through the provider seam
// (aiwatchos::weather_set_provider). The blocking HTTPS fetch runs on
// this component's own task, never on the UI loop: fetch on start, then
// every AIWATCHOS_WEATHER_REFRESH_MIN minutes while WiFi is configured,
// plus whenever the app nudges a refresh and the data is stale. Failures
// keep the last-good reading and report an honest status — nothing here
// ever fabricates a number.
//
// NWS needs no API key; its policy asks only for an identifying
// User-Agent, which the service sends on every request. Lookup chain
// (documented NWS flow):
//   GET /points/{lat},{lon}            -> properties.observationStations
//   GET <observationStations>?limit=1  -> features[0] stationIdentifier
//   GET /stations/{id}/observations/latest
// The resolved latest-observation URL is cached in RAM after the first
// successful chain walk and re-derived from scratch on any failure.
#pragma once

#include <cstddef>

#include "aiwatchos/weather_app.hpp"  // WeatherSnapshot

namespace aiwatchos_weather {

// Converts a Celsius temperature to whole Fahrenheit (x9/5+32, rounded
// to the nearest int) — the display policy for the watch face.
int weather_fahrenheit_from_celsius(double celsius);

// Extracts properties.observationStations (the station-list URL) from a
// /points response body. Field-wise scan, no JSON DOM (see
// weather_parse.cpp for why). False on malformed/truncated input.
bool weather_nws_parse_stations_url(const char* json, char* out_url,
                                    size_t cap);

// Extracts the first station's properties.stationIdentifier from an
// observation-stations list response. False on malformed input.
bool weather_nws_parse_station_id(const char* json, char* out_id,
                                  size_t cap);

// Extracts the current reading from a /observations/latest response:
//   temperature.value (deg C)  -> *out_temp_f as whole deg F. REQUIRED:
//     an explicit null (station lagging) or a missing/malformed field
//     fails the whole parse — never substitute forecast values.
//   relativeHumidity.value (%) -> *out_humidity_pct, only when present
//     and non-null (*out_has_humidity); a null humidity is a valid
//     reading that simply has no humidity to show.
//   textDescription            -> *out_description (NWS's own words,
//     e.g. "Partly Cloudy"); optional — absent leaves an empty string.
bool weather_nws_parse_observation(const char* json, float* out_temp_f,
                                   int* out_humidity_pct,
                                   bool* out_has_humidity,
                                   char* out_description,
                                   size_t desc_cap,
                                   bool* out_lagging);
// out_lagging (may be nullptr) distinguishes WHY a parse returned
// false: true means the document IS a valid observation but the
// station reports temperature.value = null (NWS lag — a real payload
// shape, captured live from KFFO on 2026-10-13: every measured field
// null, textDescription populated). Callers should treat that as
// "no new measurement", not as a corrupt response. When out_lagging
// is set, out_description still carries the parsed textDescription.

// Starts the fetch task and registers the provider with the OS weather
// app. Idempotent. Call once from app_main after aiwatchos_net::net_start().
void weather_service_start();

// The last published snapshot (same data the provider hands the OS
// weather app) — for diagnostics such as MDB health. False before the
// service starts.
bool weather_service_snapshot(aiwatchos::WeatherSnapshot* out);

}  // namespace aiwatchos_weather
