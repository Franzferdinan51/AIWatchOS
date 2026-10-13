// Host tests for the weather data path: WMO code -> label mapping and the
// staleness rule (components/aiwatchos_os/src/weather_data.cpp) plus the
// Open-Meteo JSON parser (src/weather_parse.cpp) driven by a REAL recorded
// API response (test/open_meteo_fixture.json, captured live 2026-10-13 for
// Huber Heights, Ohio — 73.6F, 63% humidity, code 0; embedded verbatim
// below so the test is hermetic).
//
// No ESP-IDF or hardware needed — plain g++, using the ESP-IDF tree's own
// cJSON (the same source the firmware links):
//
//   IDF=~/esp-idf   # any ESP-IDF 5.x checkout
//   g++ -std=c++17 -fno-exceptions -fno-rtti \
//       -I components/aiwatchos_os/include \
//       -I components/aiwatchos_weather/include \
//       -I $IDF/components/json/cJSON \
//       components/aiwatchos_os/src/weather_data.cpp \
//       components/aiwatchos_weather/src/weather_parse.cpp \
//       $IDF/components/json/cJSON/cJSON.c \
//       components/aiwatchos_weather/test/weather_logic_test.cpp \
//       -o /tmp/weather_logic_test && /tmp/weather_logic_test
#include "aiwatchos/weather_app.hpp"              // weather_condition_text, weather_is_stale
#include "aiwatchos_weather/weather_service.hpp"  // weather_parse_current

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// Verbatim copy of test/open_meteo_fixture.json (see header comment).
static const char kFixture[] =
    "{\"latitude\":39.858624,\"longitude\":-84.1436,"
    "\"generationtime_ms\":10.793328285217285,\"utc_offset_seconds\":0,"
    "\"timezone\":\"GMT\",\"timezone_abbreviation\":\"GMT\","
    "\"elevation\":299.0,\"current_units\":{\"time\":\"iso8601\","
    "\"interval\":\"seconds\",\"temperature_2m\":\"\xC2\xB0" "F\","
    "\"relative_humidity_2m\":\"%\",\"weather_code\":\"wmo code\"},"
    "\"current\":{\"time\":\"2026-10-13T15:15\",\"interval\":900,"
    "\"temperature_2m\":73.6,\"relative_humidity_2m\":63,"
    "\"weather_code\":0}}";

static bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

int main() {
    // --- WMO code -> label: every documented code group -----------------
    struct Case { int code; const char* label; };
    const Case kCases[] = {
        {0, "Clear"}, {1, "Mostly Clear"}, {2, "Partly Cloudy"},
        {3, "Overcast"}, {45, "Fog"}, {48, "Fog"},
        {51, "Drizzle"}, {53, "Drizzle"}, {55, "Drizzle"},
        {56, "Freezing Drizzle"}, {57, "Freezing Drizzle"},
        {61, "Rain"}, {63, "Rain"}, {65, "Rain"},
        {66, "Freezing Rain"}, {67, "Freezing Rain"},
        {71, "Snow"}, {73, "Snow"}, {75, "Snow"}, {77, "Snow Grains"},
        {80, "Rain Showers"}, {81, "Rain Showers"}, {82, "Rain Showers"},
        {85, "Snow Showers"}, {86, "Snow Showers"},
        {95, "Thunderstorm"}, {96, "Thunderstorm Hail"},
        {99, "Thunderstorm Hail"},
        // Undocumented / out-of-range codes must not invent a label.
        {4, "Unknown"}, {49, "Unknown"}, {100, "Unknown"}, {-1, "Unknown"},
    };
    for (const Case& c : kCases) {
        CHECK(std::strcmp(aiwatchos::weather_condition_text(c.code),
                          c.label) == 0);
    }

    // --- Parse: the recorded real response -------------------------------
    float temp_f = 0;
    int humidity = -1, code = -1;
    CHECK(aiwatchos_weather::weather_parse_current(kFixture, &temp_f,
                                                   &humidity, &code));
    CHECK(near(temp_f, 73.6f));
    CHECK(humidity == 63);
    CHECK(code == 0);
    CHECK(std::strcmp(aiwatchos::weather_condition_text(code), "Clear") == 0);

    // --- Parse: malformed / partial input is rejected whole --------------
    CHECK(!aiwatchos_weather::weather_parse_current("", &temp_f, &humidity, &code));
    CHECK(!aiwatchos_weather::weather_parse_current("{", &temp_f, &humidity, &code));
    CHECK(!aiwatchos_weather::weather_parse_current("{}", &temp_f, &humidity, &code));
    CHECK(!aiwatchos_weather::weather_parse_current("null", &temp_f, &humidity, &code));
    CHECK(!aiwatchos_weather::weather_parse_current(nullptr, &temp_f, &humidity, &code));
    CHECK(!aiwatchos_weather::weather_parse_current(kFixture, nullptr, &humidity, &code));

    std::string truncated(kFixture, 60);   // cut mid-object
    CHECK(!aiwatchos_weather::weather_parse_current(truncated.c_str(), &temp_f,
                                                    &humidity, &code));

    std::string bad_type = kFixture;       // temperature as a string
    const std::string needle = "\"temperature_2m\":73.6";
    size_t pos = bad_type.find(needle);
    CHECK(pos != std::string::npos);
    bad_type.replace(pos, needle.size(), "\"temperature_2m\":\"hot\"");
    CHECK(!aiwatchos_weather::weather_parse_current(bad_type.c_str(), &temp_f,
                                                    &humidity, &code));

    std::string missing = kFixture;        // humidity field removed entirely
    const std::string hfield = "\"relative_humidity_2m\":63,";
    pos = missing.find(hfield);
    CHECK(pos != std::string::npos);
    missing.erase(pos, hfield.size());
    CHECK(!aiwatchos_weather::weather_parse_current(missing.c_str(), &temp_f,
                                                    &humidity, &code));

    // A snowy response parses + labels through the same path.
    const char* kSnow =
        "{\"current\":{\"temperature_2m\":28.04,"
        "\"relative_humidity_2m\":81,\"weather_code\":73}}";
    CHECK(aiwatchos_weather::weather_parse_current(kSnow, &temp_f, &humidity,
                                                   &code));
    CHECK(near(temp_f, 28.04f));
    CHECK(humidity == 81);
    CHECK(code == 73);
    CHECK(std::strcmp(aiwatchos::weather_condition_text(code), "Snow") == 0);

    // --- Staleness rule ----------------------------------------------------
    const uint32_t kInterval = 30u * 60000u;   // default refresh: 30 min
    const uint64_t kFetched = 100000000ull;
    CHECK(!aiwatchos::weather_is_stale(kFetched, kFetched, kInterval));
    CHECK(!aiwatchos::weather_is_stale(kFetched + kInterval, kFetched,
                                       kInterval));   // exactly one interval: fresh
    CHECK(aiwatchos::weather_is_stale(kFetched + kInterval + 1, kFetched,
                                      kInterval));    // past it: stale
    CHECK(aiwatchos::weather_is_stale(kFetched + 60ull * 60000ull, kFetched,
                                      kInterval));
    CHECK(aiwatchos::weather_is_stale(kFetched, 0, kInterval));  // never fetched
    CHECK(!aiwatchos::weather_is_stale(kFetched - 5000, kFetched,
                                       kInterval));   // clock skew: not stale

    if (g_failures == 0) std::printf("weather_logic_test: ALL PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
