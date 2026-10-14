// AIWatchOS app wrapper for the Hermes Gadget Protocol v1 client.
//
// The protocol core (identity, HMAC auth, pairing, conversation, heartbeat,
// OTA) lives in hermes_proto.hpp and runs identically on device and in host
// tests. This header is the OS-facing surface: app lifecycle, the device
// health line, and the small API the rest of the OS (and the host tests)
// use to talk to the client.
#pragma once

#include "aiwatchos/app.hpp"
#include "aiwatchos/hal.hpp"
#include "hermes_proto.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace aiwatchos_hermes {

// Backwards-compatible alias used by existing callers/tests.
using HermesState = HermesView;

// Device-health line for the dashboard, e.g. "BAT 85% CHG UP 12:34 LINK
// ONLINE". Battery reads "--" when the PMU reports unknown (percent 255).
// LINK reflects the REAL client state: OFFLINE / CONNECTING / PAIRING /
// ONLINE / AUTH FAIL — never a hardcoded value.
std::string hermes_health_line();

// Which hub endpoint the link layer is riding: "LAN" (home) or "REMOTE"
// (away-from-home, the hub host's Tailscale Funnel over wss). Shown as
// the health line's VIA tag; the underlying failover policy is the pure
// HermesFailover machine in hermes_proto.hpp.
const char* hermes_link_endpoint_tag();

// Uptime as MM:SS below one hour, HH:MM above. Writes into out (always NUL-
// terminated) and returns out.
char* hermes_uptime_str(uint64_t now_ms, char* out, size_t len);

// --- Live client API -------------------------------------------------------

// Current link state of the protocol client.
HermesLinkState hermes_link_state();

// True once the server has welcomed us AND the owner has approved pairing.
bool hermes_paired();

// The 8-character pairing code while awaiting approval ("" otherwise), and
// the full approve command the server sent ("hermes pairing approve ...").
// Returned by value: the bytes are copied under the client lock, so the
// link task can never mutate them under the caller.
std::string hermes_pairing_code();
std::string hermes_pairing_command();

// Queue/send a chat text. When the link is down or pairing is pending the
// text joins a bounded FIFO outbox (depth 4, oldest evicted) that is
// persisted across reboots and flushed in order once the session is
// paired. Returns false only for empty text.
bool hermes_send_text(const char* text);

// Answer the active yes/no prompt (false when none is active).
bool hermes_answer_prompt(bool yes);

// Buddy-harvest device-key reset: erase the stored identity, mint a
// fresh key, and clear the AuthFailed latch so the link re-enrolls
// (new pairing code). Fired by the BOOT long-hold on the AUTH FAIL
// screen; harmless to call in other states.
bool hermes_forget_device();
// H5: start a fresh Hermes session (sends session.new; see
// HermesClient::new_session). False when not paired+linked.
bool hermes_new_session();

// Borrowed reference to everything the renderer draws. The view lives
// inside the client (PSRAM on device); callers must not copy it (it is
// ~35 KB with the H1 history ring) and should hold no assumptions
// across client events.
const HermesView& hermes_view();

// Drive the client from a periodic context (TTL expiry, heartbeat-death
// detection, battery state). The app tick calls this on device and in
// tests; the link task also calls it so the link stays healthy while the
// Hermes app is not in the foreground.
void hermes_pump(uint64_t now_ms);

// Initialize the Hermes app: builds the protocol client, loads (or creates)
// the device identity, and starts the link layer (device builds).
void hermes_init();

// Tick — called every ~40 ms by AppManager::tick() while foregrounded.
void hermes_tick(uint32_t elapsed_ms);

// Render — draws the Hermes conversation screen: link state, pairing code,
// conversation (status/transcript/reply), prompt and display cards, OTA
// progress, and the device-health battery bar.
void hermes_render(aiwatchos::Framebuffer& fb);

// Touch input: hold bottom-center = TALK (push-to-talk), release sends;
// YES/NO targets while a prompt is up; tap dismisses a display card;
// press near the top = CANCEL.
bool hermes_on_touch(const aiwatchos::TouchEvent& event);

// Build an OS App descriptor wrapping all callbacks above (App ID "hermes").
// Takes the Hal the adapter drives (Board on device, a fake in tests),
// matching make_muse_app.
aiwatchos::App make_hermes_app(aiwatchos::Hal& hal = aiwatchos::Board::instance());

// --- Host-test hooks -------------------------------------------------------
// The host build has no WebSocket transport; these drive the REAL protocol
// client exactly as the device link layer does, and capture what it sends.
// They mirror the repo's existing test seams (board_inject_touch etc.).

void hermes_test_reset(const uint8_t key[32]);   // fresh client with a known key
void hermes_test_restore();                       // run the boot restore (persisted state)
void hermes_test_transport_up();                  // simulate WS connect
void hermes_test_transport_down();                // simulate WS disconnect
void hermes_test_inject_text(const char* json);   // one inbound text frame
void hermes_test_inject_binary(const uint8_t* data, size_t len);
void hermes_test_talk_feed(const int16_t* pcm, size_t frames);   // mic pump stand-in
// Consume the handshake watchdog's link-recycle request (host tests drive
// the same take-once path the link task uses on device).
bool hermes_test_take_cycle_request();
size_t hermes_test_outbound_count();
bool hermes_test_outbound_is_binary(size_t i);
std::string hermes_test_outbound_text(size_t i);  // text frame i ("" if binary)
void hermes_test_clear_outbound();
// Bytes the OTA flash sink captured (host memory sink); returns the count
// and, when out is non-null, a pointer to the captured image.
size_t hermes_test_ota_captured(const uint8_t** out);

}  // namespace aiwatchos_hermes
