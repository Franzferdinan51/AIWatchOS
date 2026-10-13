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
void hermes_link_erase_key();   // forget the stored identity (key reset)
// Buddy-harvest persistence: the offline outbox ring and the last
// conversation pair, stored in the same NVS namespace as the key.
void hermes_link_save_outbox(const char* const* texts, int count);
int hermes_link_load_outbox(char (*out)[512], int max);   // returns count
void hermes_link_save_conversation(const char* transcript, const char* reply);
bool hermes_link_load_conversation(char* t, size_t tcap, char* r, size_t rcap);
void hermes_link_clear_conversation();
// Cut the current reconnect backoff short (buddy's wakeup channel):
// foregrounding the app or queueing a text pokes the pump loop.
void hermes_link_wake_reconnect();
// Endpoint diagnostics for the health line: "LAN" or "REMOTE" — the
// endpoint of the link layer's latest connection attempt (the live
// endpoint while connected). Host stub reports "LAN".
const char* hermes_link_endpoint_tag();
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
void hermes_core_set_battery(int pct, int charging);   // charging: -1/0/1
bool hermes_core_ota_reboot_pending();
void hermes_core_ota_reboot_consumed();

}  // namespace aiwatchos_hermes
