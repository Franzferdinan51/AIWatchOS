// Regression test for the 2026-10-13 first-boot panic: net_save_config
// reboot-looped the watch ~57 s into boot (Guru Meditation
// LoadProhibited). Root cause in net_config.cpp: the local
// string-overload helper nvs_set_str(nvs_handle_t, const char*,
// const std::string&) HIDES the global ESP-IDF ::nvs_set_str for
// unqualified lookup, so the helper's inner call recursed into itself
// — one std::string allocation per level via operator new — until the
// stack blew. Compiled against the pre-fix source, this test never
// returns from net_save_config (stack overflow); against the fixed
// source (::nvs_set_str / ::nvs_erase_key) it verifies every key lands
// in the C NVS API exactly once, both helper branches (set + erase).
//
//   g++ -std=c++17 -fno-exceptions -fno-rtti \
//       -I components/aiwatchos_net/test/stubs \
//       -I components/aiwatchos_net/include \
//       components/aiwatchos_net/src/net_config.cpp \
//       components/aiwatchos_net/src/net_proto.cpp \
//       components/aiwatchos_net/test/net_config_test.cpp \
//       -o /tmp/net_config_test && /tmp/net_config_test
#include "aiwatchos_net/net.hpp"

#include "nvs.h"  // test/stubs — the C API implemented below

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

// ---- NVS C API stubs (declared by test/stubs/nvs.h) ----------------
static std::map<std::string, std::string> g_strs;
static std::map<std::string, uint8_t> g_u8s;
static int g_set_str_calls = 0;
static int g_erase_calls = 0;
static int g_set_u8_calls = 0;
static int g_commit_calls = 0;

extern "C" esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t* out) {
    *out = 1;
    return ESP_OK;
}
extern "C" void nvs_close(nvs_handle_t) {}
extern "C" esp_err_t nvs_get_str(nvs_handle_t, const char* key, char* out,
                                 size_t* len) {
    auto it = g_strs.find(key);
    if (it == g_strs.end()) return ESP_ERR_NVS_NOT_FOUND;
    const size_t need = it->second.size() + 1;
    if (out) {
        if (*len < need) return ESP_FAIL;
        std::memcpy(out, it->second.c_str(), need);
    }
    *len = need;
    return ESP_OK;
}
extern "C" esp_err_t nvs_set_str(nvs_handle_t, const char* key,
                                 const char* value) {
    ++g_set_str_calls;
    g_strs[key] = value;
    return ESP_OK;
}
extern "C" esp_err_t nvs_get_u8(nvs_handle_t, const char* key, uint8_t* out) {
    auto it = g_u8s.find(key);
    if (it == g_u8s.end()) return ESP_ERR_NVS_NOT_FOUND;
    *out = it->second;
    return ESP_OK;
}
extern "C" esp_err_t nvs_set_u8(nvs_handle_t, const char* key, uint8_t v) {
    ++g_set_u8_calls;
    g_u8s[key] = v;
    return ESP_OK;
}
extern "C" esp_err_t nvs_erase_key(nvs_handle_t, const char* key) {
    ++g_erase_calls;
    g_strs.erase(key);
    g_u8s.erase(key);
    return ESP_OK;
}
extern "C" esp_err_t nvs_commit(nvs_handle_t) {
    ++g_commit_calls;
    return ESP_OK;
}

// ---- checks ---------------------------------------------------------
static int g_failures = 0;
#define CHECK(cond)                                                   \
    do {                                                              \
        if (!(cond)) {                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                             \
        }                                                             \
    } while (0)

int main() {
    using namespace aiwatchos_net;

    // Kconfig defaults load (stub sdkconfig.h values).
    CHECK(net_config().wifi_ssid == "HomeWiFi");

    NetConfig cfg = net_config();
    cfg.wifi_ssid = "HomeWiFi";
    cfg.wifi_pass = "placeholder-pass";
    cfg.hatch_token = "tok-placeholder";
    cfg.vm_token = "vm-placeholder";
    cfg.vm_id = "vm-1";
    cfg.host = "host.example.invalid";
    cfg.domain = "example.invalid";
    cfg.tts_url = "https://tts.example.invalid/v1/audio/speech";
    cfg.tts_key = "key-placeholder";
    cfg.tts_model = "model-x";
    cfg.tts_voice = "voice-y";
    cfg.tts_speak = true;
    cfg.hermes_remote_host = "your-mac.your-tailnet.ts.net";
    cfg.hermes_remote_port = 8443;
    cfg.hermes_remote_enabled = true;

    // The save itself is the regression: pre-fix it never returned.
    CHECK(net_save_config(cfg));
    CHECK(g_set_str_calls == 13);   // 13 string keys, exactly once each (11 + hhost2 + hport2)
    CHECK(g_set_u8_calls == 2);     // ttsspeak + hrem
    CHECK(g_commit_calls == 1);
    CHECK(g_strs["ssid"] == "HomeWiFi");
    CHECK(g_strs["token"] == "tok-placeholder");
    CHECK(g_strs["ttskey"] == "key-placeholder");
    CHECK(g_u8s["ttsspeak"] == 1);
    CHECK(g_strs["hhost2"] == "your-mac.your-tailnet.ts.net");
    CHECK(g_strs["hport2"] == "8443");
    CHECK(g_u8s["hrem"] == 1);
    CHECK(net_config().hatch_token == "tok-placeholder");

    // Empty string -> the helper's erase branch (also ::-qualified).
    cfg.wifi_pass.clear();
    g_set_str_calls = 0;
    g_erase_calls = 0;
    CHECK(net_save_config(cfg));
    CHECK(g_set_str_calls == 12);
    CHECK(g_erase_calls == 1);
    CHECK(g_strs.count("pass") == 0);

    if (g_failures == 0) std::printf("net_config_test: all checks passed\n");
    return g_failures ? 1 : 0;
}
