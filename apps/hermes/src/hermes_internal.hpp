// Internal glue between the Hermes app adapter (hermes_adapter.cpp) and the
// platform link layer (hermes_link.cpp). Not part of the public app API.
#pragma once

#include <cstddef>
#include <cstdint>

namespace aiwatchos_hermes {

// --- adapter -> link (device transport; hermes_link.cpp provides host stubs)
bool hermes_link_start();   // NVS identity + WiFi + WebSocket task (device)
bool hermes_link_send_text(const char* json, size_t len);
bool hermes_link_send_binary(const uint8_t* data, size_t len);
bool hermes_link_load_key(uint8_t key[32]);   // false when no stored key
void hermes_link_store_key(const uint8_t key[32]);
void hermes_link_fill_random(uint8_t* out, size_t len);
bool hermes_link_ota_begin(uint32_t size);
bool hermes_link_ota_write(const uint8_t* data, size_t len);
bool hermes_link_ota_finish();
void hermes_link_ota_abort();
void* hermes_link_alloc_big(size_t len);   // device: PSRAM, fail-closed
void hermes_link_free_big(void* ptr);
// Host-test introspection of the OTA capture sink (device: size 0/nullptr).
size_t hermes_link_test_ota_size();
const uint8_t* hermes_link_test_ota_data();
void hermes_link_test_ota_reset();

// --- link -> adapter core (the link task drives the protocol client) -------
void hermes_core_transport_up();
void hermes_core_transport_down();
void hermes_core_transport_text(const char* json, size_t len);
void hermes_core_transport_binary(const uint8_t* data, size_t len);
void hermes_core_pump();                    // client.tick + housekeeping
bool hermes_core_wants_reconnect();
bool hermes_core_talk_active();
void hermes_core_talk_feed(const int16_t* pcm, size_t frames);
void hermes_core_set_battery(int pct);
bool hermes_core_ota_reboot_pending();
void hermes_core_ota_reboot_consumed();

}  // namespace aiwatchos_hermes
