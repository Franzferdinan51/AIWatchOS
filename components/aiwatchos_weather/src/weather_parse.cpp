// National Weather Service (api.weather.gov) response parsing — see
// weather_service.hpp. Kept free of ESP-IDF and cJSON so the host test
// builds this exact file with plain g++.
//
// Why field-wise scanning instead of a JSON DOM: NWS GeoJSON responses
// are large (the observation-stations list runs ~75 KB unabridged) and
// this parser runs against a fixed 16 KB PSRAM buffer on a device whose
// internal DRAM margin is measured in tens of KB. Building a cJSON tree
// of a 16 KB document would transiently allocate many small nodes; the
// extractors below instead walk straight to the handful of fields the
// snapshot needs. Every scan is bounded by the buffer's NUL terminator
// and every failure returns false — truncated or malformed input can
// never produce a partial reading.
#include "aiwatchos_weather/weather_service.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace aiwatchos_weather {
namespace {

const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    return p;
}

// Finds `"key"` followed by optional whitespace and ':' at or after
// `start`. Returns a pointer just past the ':', or nullptr.
const char* find_key(const char* start, const char* key) {
    if (!start || !key) return nullptr;
    const size_t klen = std::strlen(key);
    for (const char* p = start; *p; ++p) {
        if (*p != '"') continue;
        if (std::strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char* q = skip_ws(p + 1 + klen + 1);
            if (*q == ':') return q + 1;
        }
    }
    return nullptr;
}

// All fields this parser needs live under the document's first
// "properties" object (points / station feature / observation alike).
const char* properties(const char* json) {
    if (!json) return nullptr;
    const char* p = find_key(json, "properties");
    return p ? p : json;
}

// Parses a JSON string whose opening quote is at or after `p` (leading
// whitespace allowed). Copies into out (cap includes the NUL), unescaping
// the standard sequences; \uXXXX degrades to '?' (NWS descriptions are
// plain ASCII in practice, and a mangled glyph beats a failed parse of
// otherwise good data — except truncation, which IS a failure: an
// unterminated string returns false).
bool parse_json_string(const char* p, char* out, size_t cap) {
    if (!p || !out || cap == 0) return false;
    p = skip_ws(p);
    if (*p != '"') return false;
    ++p;
    size_t n = 0;
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\') {
            ++p;
            if (!*p) return false;
            switch (*p) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case '/': c = '/'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case 'u': {
                    // Skip the 4 hex digits; emit a placeholder.
                    for (int i = 0; i < 4; ++i) {
                        if (p[1] && std::isxdigit(static_cast<unsigned char>(p[1]))) ++p;
                        else return false;
                    }
                    c = '?';
                    break;
                }
                default: return false;
            }
        }
        if (n + 1 < cap) out[n++] = c;
        ++p;
    }
    if (*p != '"') return false;   // unterminated (e.g. truncated body)
    out[n] = '\0';
    return true;
}

// Extracts the string value of `key` found at or after `start`.
bool string_field(const char* start, const char* key, char* out, size_t cap) {
    const char* v = find_key(start, key);
    if (!v) return false;
    return parse_json_string(v, out, cap);
}

// Given a pointer just past an object's opening '{', returns a pointer
// to its matching '}' (string-aware), or nullptr if unbalanced.
const char* object_end(const char* p) {
    int depth = 1;
    bool in_str = false, esc = false;
    for (; *p; ++p) {
        if (in_str) {
            if (esc) esc = false;
            else if (*p == '\\') esc = true;
            else if (*p == '"') in_str = false;
            continue;
        }
        if (*p == '"') in_str = true;
        else if (*p == '{') ++depth;
        else if (*p == '}' && --depth == 0) return p;
    }
    return nullptr;
}

// Reads `"<obj_key>": { ..., "value": <number|null>, ... }` under `base`.
// Returns false if the object or its value is missing/malformed; on
// success *is_null distinguishes an explicit JSON null from a number.
bool object_number_field(const char* base, const char* obj_key, double* out,
                         bool* is_null) {
    const char* v = find_key(base, obj_key);
    if (!v) return false;
    v = skip_ws(v);
    if (*v != '{') return false;
    const char* end = object_end(v + 1);
    if (!end) return false;
    const char* val = find_key(v + 1, "value");
    if (!val || val > end) return false;
    val = skip_ws(val);
    if (std::strncmp(val, "null", 4) == 0) {
        *is_null = true;
        return true;
    }
    char* endp = nullptr;
    double d = std::strtod(val, &endp);
    if (endp == val) return false;
    *is_null = false;
    *out = d;
    return true;
}

}  // namespace

int weather_fahrenheit_from_celsius(double celsius) {
    return static_cast<int>(std::lround(celsius * 9.0 / 5.0 + 32.0));
}

bool weather_nws_parse_stations_url(const char* json, char* out_url,
                                    size_t cap) {
    if (!json || !out_url || cap == 0) return false;
    out_url[0] = '\0';
    char url[256];
    if (!string_field(properties(json), "observationStations", url,
                      sizeof(url))) {
        return false;
    }
    if (url[0] == '\0' || std::strlen(url) + 1 > cap) return false;
    std::strcpy(out_url, url);
    return true;
}

bool weather_nws_parse_station_id(const char* json, char* out_id,
                                  size_t cap) {
    if (!json || !out_id || cap == 0) return false;
    out_id[0] = '\0';
    // The stations response is a FeatureCollection; the first feature's
    // properties carry the nearest station (the service asks for
    // ?limit=1, but first-feature is the rule either way).
    char id[32];
    if (!string_field(properties(json), "stationIdentifier", id, sizeof(id))) {
        return false;
    }
    if (id[0] == '\0' || std::strlen(id) + 1 > cap) return false;
    std::strcpy(out_id, id);
    return true;
}

bool weather_nws_parse_observation(const char* json, float* out_temp_f,
                                   int* out_humidity_pct,
                                   bool* out_has_humidity,
                                   char* out_description,
                                   size_t desc_cap) {
    if (!json || !out_temp_f || !out_humidity_pct || !out_has_humidity ||
        !out_description || desc_cap == 0) {
        return false;
    }
    out_description[0] = '\0';
    *out_has_humidity = false;
    const char* props = properties(json);

    // Temperature is the one mandatory datum: a null temperature means
    // the station has not reported (NWS stations lag) — the fetch fails
    // honestly rather than substituting anything else.
    double temp_c = 0;
    bool temp_null = false;
    if (!object_number_field(props, "temperature", &temp_c, &temp_null)) {
        return false;
    }
    if (temp_null) return false;

    // Humidity may legitimately be null: report "not measured" instead
    // of inventing 0%.
    double hum = 0;
    bool hum_null = false;
    if (object_number_field(props, "relativeHumidity", &hum, &hum_null) &&
        !hum_null) {
        *out_humidity_pct = static_cast<int>(std::lround(hum));
        *out_has_humidity = true;
    }

    // Description is display text straight from NWS ("Partly Cloudy").
    // Optional: absent means an empty string, not a failed reading.
    char desc[64];
    if (string_field(props, "textDescription", desc, sizeof(desc))) {
        std::strncpy(out_description, desc, desc_cap - 1);
        out_description[desc_cap - 1] = '\0';
    }

    *out_temp_f = static_cast<float>(weather_fahrenheit_from_celsius(temp_c));
    return true;
}

}  // namespace aiwatchos_weather
