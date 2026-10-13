// Account API client — internal to aiwatchos_net.
//
// Port of muse-gadget-206's muse_account_api.c (same author ecosystem):
// GET https://api.muse.ai/fetch_vms with the account device token,
// pick the wanted (or default) VM, return its per-VM credential + wss URL.
// Adapted to C++ / ESP-IDF 5.3; behavior (retries, 401/403 = auth error,
// vm_id derivation from the ws URL) matches the reference.
#pragma once

#include <string>

namespace aiwatchos_net {

struct VmInfo {
    bool ok = false;
    bool auth_failed = false;   // 401/403: the device token is wrong/revoked
    std::string vm_id;
    std::string token;          // per-VM bearer token for /v1/noise
    std::string ws_url;         // wss URL the account advertises for this VM
    std::string name;
};

VmInfo account_find_vm(const std::string& device_token,
                       const std::string& want_vm);

}  // namespace aiwatchos_net
