// Host stub for the ESP-IDF NVS C API — just the surface net_config.cpp
// uses. The implementations live in net_config_test.cpp, backed by an
// in-memory map with call counters.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int32_t esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_NVS_NOT_FOUND 0x1102
typedef unsigned int nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;
esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* out);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_str(nvs_handle_t handle, const char* key, char* out, size_t* len);
esp_err_t nvs_set_str(nvs_handle_t handle, const char* key, const char* value);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char* key, uint8_t* out);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char* key, uint8_t value);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key);
esp_err_t nvs_commit(nvs_handle_t handle);
#ifdef __cplusplus
}
#endif
