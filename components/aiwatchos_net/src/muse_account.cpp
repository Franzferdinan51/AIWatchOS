// See muse_account.hpp. Faithful port of the gadget firmware's
// muse_account_api.c (fetch_vms), minus the pieces AIWatchOS doesn't need.
#include "muse_account.hpp"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "aiwatchos_net/net_proto.hpp"

namespace aiwatchos_net {
namespace {

const char* kTag = "aiw-account";
const char* kFetchVmsUrl = "https://api.muse.ai/fetch_vms";
constexpr size_t kMaxBody = 128 * 1024;

struct BodySink {
    std::string* out;
    bool overflow = false;
};

esp_err_t on_http_event(esp_http_client_event_t* evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0)
        return ESP_OK;
    auto* sink = static_cast<BodySink*>(evt->user_data);
    if (!sink || !sink->out) return ESP_OK;
    if (sink->out->size() + static_cast<size_t>(evt->data_len) > kMaxBody) {
        sink->overflow = true;
        return ESP_FAIL;
    }
    sink->out->append(static_cast<const char*>(evt->data),
                      static_cast<size_t>(evt->data_len));
    return ESP_OK;
}

const cJSON* obj_get(const cJSON* o, const char* a, const char* b = nullptr) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, a);
    if (!v && b) v = cJSON_GetObjectItemCaseSensitive(o, b);
    return v;
}
bool json_str(const cJSON* o, const char* a, const char* b, std::string* out) {
    const cJSON* v = obj_get(o, a, b);
    if (cJSON_IsString(v) && v->valuestring) {
        *out = v->valuestring;
        return true;
    }
    return false;
}

bool parse_vm_list(const std::string& body, const std::string& want,
                   VmInfo* out) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return false;
    const cJSON* list = obj_get(root, "vm_list", "vms");
    bool found = false;
    if (cJSON_IsArray(list)) {
        const cJSON* first_usable = nullptr;
        const cJSON* chosen = nullptr;
        int n = cJSON_GetArraySize(list);
        for (int i = 0; i < n; ++i) {
            const cJSON* vm = cJSON_GetArrayItem(list, i);
            if (!cJSON_IsObject(vm)) continue;
            std::string tok;
            if (!json_str(vm, "vm_auth_token", "token", &tok) || tok.empty())
                continue;
            if (!first_usable) first_usable = vm;
            std::string id, url;
            json_str(vm, "vm_ws_url", "vm_url", &url);
            if (!json_str(vm, "vm_id", "id", &id) && !url.empty()) {
                std::string host = proto::host_from_ws_url(url);
                size_t dot = host.find('.');
                id = (dot == std::string::npos) ? host : host.substr(0, dot);
            }
            const cJSON* dflt = obj_get(vm, "default", "is_default");
            bool is_default = cJSON_IsTrue(dflt);
            if (!want.empty() && id == want) { chosen = vm; break; }
            if (want.empty() && is_default) { chosen = vm; break; }
        }
        if (!chosen) chosen = first_usable;
        if (chosen) {
            std::string tok, id, url, name;
            json_str(chosen, "vm_auth_token", "token", &tok);
            json_str(chosen, "vm_ws_url", "vm_url", &url);
            if (!json_str(chosen, "vm_id", "id", &id) && !url.empty()) {
                std::string host = proto::host_from_ws_url(url);
                size_t dot = host.find('.');
                id = (dot == std::string::npos) ? host : host.substr(0, dot);
            }
            json_str(chosen, "vm_name", "name", &name);
            out->vm_id = id;
            out->token = tok;
            out->ws_url = url;
            out->name = name;
            found = !tok.empty();
        }
    }
    cJSON_Delete(root);
    return found;
}

}  // namespace

VmInfo account_find_vm(const std::string& device_token,
                       const std::string& want_vm) {
    VmInfo out;
    if (device_token.empty()) return out;

    for (int attempt = 0; attempt < 3 && !out.ok; ++attempt) {
        if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
        std::string body;
        BodySink sink{&body, false};
        esp_http_client_config_t cfg{};
        cfg.url = kFetchVmsUrl;
        cfg.event_handler = &on_http_event;
        cfg.user_data = &sink;
        cfg.timeout_ms = 15000;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.disable_auto_redirect = true;
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) continue;
        std::string auth = "Bearer " + device_token;
        esp_http_client_set_header(client, "Authorization", auth.c_str());
        esp_http_client_set_header(client, "X-API-Version", "1.0.0");
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "fetch_vms attempt %d failed: %s", attempt,
                     esp_err_to_name(err));
            continue;
        }
        if (status == 401 || status == 403) {
            ESP_LOGE(kTag, "fetch_vms auth rejected (HTTP %d)", status);
            out.auth_failed = true;
            return out;
        }
        if (status != 200) {
            ESP_LOGW(kTag, "fetch_vms HTTP %d", status);
            continue;
        }
        if (parse_vm_list(body, want_vm, &out)) {
            out.ok = true;
            ESP_LOGI(kTag, "resolved vm '%s' (%s)", out.vm_id.c_str(),
                     out.name.c_str());
        } else {
            ESP_LOGW(kTag, "fetch_vms: no usable vm in response");
        }
    }
    return out;
}

}  // namespace aiwatchos_net
