// Configuration resolution: Kconfig build-time defaults, overridden by the
// NVS namespace "aiwnet" (the runtime provisioning path). See net.hpp.
#include "aiwatchos_net/net.hpp"

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "aiwatchos_net/net_proto.hpp"

namespace aiwatchos_net {
namespace {

const char* kTag = "aiw-net-cfg";
const char* kNs = "aiwnet";

NetConfig g_cfg;
bool g_loaded = false;

std::string nvs_read_str(nvs_handle_t h, const char* key,
                        const std::string& fallback) {
    size_t len = 0;
    if (::nvs_get_str(h, key, nullptr, &len) != ESP_OK || len <= 1) {
        return fallback;
    }
    std::string v(len - 1, '\0');
    if (::nvs_get_str(h, key, v.data(), &len) != ESP_OK) return fallback;
    return v;
}

void load_once() {
    if (g_loaded) return;
    g_loaded = true;
    g_cfg.wifi_ssid = CONFIG_AIWATCHOS_NET_WIFI_SSID;
    g_cfg.wifi_pass = CONFIG_AIWATCHOS_NET_WIFI_PASSWORD;
    g_cfg.hatch_token = CONFIG_AIWATCHOS_NET_HATCH_TOKEN;
    g_cfg.vm_token = CONFIG_AIWATCHOS_NET_HATCH_VM_TOKEN;
    g_cfg.vm_id = CONFIG_AIWATCHOS_NET_HATCH_VM;
    g_cfg.host = CONFIG_AIWATCHOS_NET_HATCH_HOST;
    g_cfg.domain = CONFIG_AIWATCHOS_NET_HATCH_DOMAIN;
    g_cfg.sntp_server = CONFIG_AIWATCHOS_NET_SNTP_SERVER;

    nvs_handle_t h = 0;
    if (nvs_open(kNs, NVS_READONLY, &h) == ESP_OK) {
        g_cfg.wifi_ssid = nvs_read_str(h, "ssid", g_cfg.wifi_ssid);
        g_cfg.wifi_pass = nvs_read_str(h, "pass", g_cfg.wifi_pass);
        g_cfg.hatch_token = nvs_read_str(h, "token", g_cfg.hatch_token);
        g_cfg.vm_token = nvs_read_str(h, "vmtoken", g_cfg.vm_token);
        g_cfg.vm_id = nvs_read_str(h, "vm", g_cfg.vm_id);
        g_cfg.host = nvs_read_str(h, "host", g_cfg.host);
        g_cfg.domain = nvs_read_str(h, "domain", g_cfg.domain);
        nvs_close(h);
    }
}

void nvs_set_str(nvs_handle_t h, const char* key, const std::string& v) {
    if (v.empty()) nvs_erase_key(h, key);
    else nvs_set_str(h, key, v.c_str());
}

}  // namespace

const NetConfig& net_config() {
    load_once();
    return g_cfg;
}

bool net_save_config(const NetConfig& cfg) {
    load_once();
    nvs_handle_t h = 0;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_str(h, "ssid", cfg.wifi_ssid);
    nvs_set_str(h, "pass", cfg.wifi_pass);
    nvs_set_str(h, "token", cfg.hatch_token);
    nvs_set_str(h, "vmtoken", cfg.vm_token);
    nvs_set_str(h, "vm", cfg.vm_id);
    nvs_set_str(h, "host", cfg.host);
    nvs_set_str(h, "domain", cfg.domain);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return false;
    g_cfg = cfg;
    ESP_LOGI(kTag, "config saved (ssid '%s', vm '%s', host '%s')",
             cfg.wifi_ssid.c_str(), cfg.vm_id.c_str(), cfg.host.c_str());
    return true;
}

bool net_apply_setup_string(const char* text) {
    proto::SetupFields f;
    if (!proto::parse_setup_string(text, &f)) return false;
    NetConfig cfg = net_config();
    if (f.has_ssid) cfg.wifi_ssid = f.ssid;
    if (f.has_pass) cfg.wifi_pass = f.pass;
    if (f.has_token) cfg.hatch_token = f.token;
    if (f.has_vm_token) cfg.vm_token = f.vm_token;
    if (f.has_vm) cfg.vm_id = f.vm;
    if (f.has_host) cfg.host = f.host;
    if (f.has_domain) cfg.domain = f.domain;
    return net_save_config(cfg);
}

}  // namespace aiwatchos_net
