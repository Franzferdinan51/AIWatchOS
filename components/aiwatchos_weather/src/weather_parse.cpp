// Open-Meteo response parsing — see weather_service.hpp. Kept free of
// ESP-IDF includes (cJSON only) so the host test builds this exact file.
#include "aiwatchos_weather/weather_service.hpp"

#include <cmath>

#include "cJSON.h"

namespace aiwatchos_weather {

bool weather_parse_current(const char* json, float* out_temp_f,
                           int* out_humidity_pct, int* out_weather_code) {
    if (!json || !out_temp_f || !out_humidity_pct || !out_weather_code) {
        return false;
    }
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;

    bool ok = false;
    const cJSON* current = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (cJSON_IsObject(current)) {
        const cJSON* temp =
            cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
        const cJSON* hum =
            cJSON_GetObjectItemCaseSensitive(current, "relative_humidity_2m");
        const cJSON* code =
            cJSON_GetObjectItemCaseSensitive(current, "weather_code");
        if (cJSON_IsNumber(temp) && cJSON_IsNumber(hum) &&
            cJSON_IsNumber(code)) {
            *out_temp_f = static_cast<float>(temp->valuedouble);
            *out_humidity_pct =
                static_cast<int>(std::lround(hum->valuedouble));
            *out_weather_code = static_cast<int>(code->valuedouble);
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

}  // namespace aiwatchos_weather
