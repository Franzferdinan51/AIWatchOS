// Shared network + Muse transport for AIWatchOS.
//
// One owner for connectivity: net_start() brings up WiFi STA + SNTP and the
// Muse link task. Everything is configuration-driven and fails soft: with
// no credentials configured the watch keeps today's behavior exactly —
// voice turns collect in the Muse app's outbox, offline.
//
// Configuration sources, in priority order:
//   1. NVS namespace "aiwnet" (written by net_save_config / the setup-string
//      seam — the runtime provisioning path for settings UI / BLE / serial)
//   2. Kconfig build-time defaults (AIWATCHOS_NET_*) — leave them empty in
//      source; they exist so a pre-provisioned build can ship ready.
// No personal credentials are compiled in anywhere.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace aiwatchos_net {

// Resolved configuration (after Kconfig + NVS merge). Plain data; the
// strings are owned by the component.
struct NetConfig {
    std::string wifi_ssid;
    std::string wifi_pass;
    std::string hatch_token;    // account device token (api.muse.ai)
    std::string vm_token;       // per-VM bearer token (skips account API)
    std::string vm_id;          // wanted VM id ("" = account default)
    std::string host;           // explicit host override ("" = derive)
    std::string domain;         // for "<vm>.<domain>" derivation
    std::string sntp_server;

    // Spoken replies (TTS): an OpenAI-shaped speech provider. All four
    // are plain config — no provider or key is baked in as mandatory.
    std::string tts_base_url;   // full API prefix, e.g. https://openrouter.ai/api
    std::string tts_api_key;    // bearer key; EMPTY = spoken replies off
    std::string tts_model;
    std::string tts_voice;

    // True when a Muse session is even attemptable: WiFi credentials and
    // either an account token or a direct VM token.
    bool muse_configured() const {
        return !wifi_ssid.empty() &&
               (!hatch_token.empty() || !vm_token.empty());
    }
    bool wifi_configured() const { return !wifi_ssid.empty(); }
    // Spoken replies are opt-in: no key, no speech — and no requests.
    bool tts_configured() const { return !tts_api_key.empty(); }
};

// Current resolved config (after net_start; before that, Kconfig defaults).
const NetConfig& net_config();

// Persists overrides to NVS and returns the merged config. Used by the
// provisioning seams; takes effect on the next net_start / WiFi reconnect
// cycle (the link notices changed config and re-resolves).
bool net_save_config(const NetConfig& cfg);

// Applies an "aiwatchos://setup?..." provisioning string (see net_proto.hpp)
// over the current config and persists it. Returns false on a bad string.
bool net_apply_setup_string(const char* text);

// --- WiFi / time -----------------------------------------------------------

enum class WifiState { kOff, kNoCredentials, kConnecting, kConnected, kFailed };
WifiState wifi_state();
const char* wifi_state_name(WifiState s);
bool wifi_connected();

// True once SNTP has disciplined the board clock at least once. The board
// clock itself moves via aiwatchos::board_set_epoch_ms (the seam in
// aiwatchos_os), called by the net manager on every sync.
bool net_time_synced();

// --- Muse link -------------------------------------------------------------

enum class LinkState {
    kOff,         // net disabled or nothing configured
    kWaitingWifi, // configured, waiting for WiFi / SNTP
    kConnecting,  // resolving VM / TLS / Noise handshake in flight
    kLinked,      // session up: subscribe stream open, turns drain
    kBackoff,     // connect failed; retrying with backoff
};
LinkState muse_link_state();
const char* muse_link_state_name(LinkState s);

// Outbox pump: when the link is up, the net task invokes the registered
// pump, which is expected to call the Muse app's muse_outbox_drain() with
// a send callback that forwards to muse_link_upload_turn(). The pump runs
// on the net task — the callback chain must be quick-or-blocking-audio-safe
// in the same way the gadget's session task is.
using OutboxPumpFn = bool (*)(void* ctx);
void muse_link_set_outbox_pump(OutboxPumpFn fn, void* ctx);

// Sends one completed voice turn (16 kHz IMA-ADPCM, the Muse app's buffer
// format) as a chat voice note and collects the text reply. Returns true
// once the note has been fully accepted by the server (the outbox may drop
// it); a reply that never arrives does not requeue the turn. Call only
// from the registered pump (net task context) while linked.
bool muse_link_upload_turn(const uint8_t* adpcm, size_t len);

// The most recent assistant reply text ("" when none yet). Thread-safe
// snapshot copy; replies are text — the hatch chat protocol has no audio
// reply stream (see the report / muse_link.cpp notes).
bool muse_link_last_reply(char* out, size_t cap);

// --- Agent status + live reply (companion M1/M2 surfaces) ------------------

// Latest agent/task status retained from the subscribe stream's
// agent.status / task.status events — payloads the parser used to drop
// after counting them for settle timing.
struct MuseAgentStatus {
    bool valid = false;    // a status event has been parsed this session
    bool busy = false;     // agent is working (firmware busy semantics)
    char code[40] = {};    // raw activity code, e.g. "searching"
    char label[96] = {};   // task label when the event carried one
    int64_t age_ms = -1;   // ms since the status last updated (-1 = none)
};
bool muse_link_agent_status(MuseAgentStatus* out);

// The reply text of the in-progress (or most recent) turn, RAW — the UI
// cleans it for display (M3). While a reply streams in, this grows delta
// by delta; muse_link_reply_streaming() says whether it is still moving.
bool muse_link_live_reply(char* out, size_t cap);
bool muse_link_reply_streaming();

// --- Spoken replies (TTS) ----------------------------------------------------
// When a speech provider is configured (NetConfig::tts_configured()),
// the link fetches an MP3 of each completed Muse reply from the
// provider and hands it to the registered sink — the Muse app
// registers a sink that plays it via muse_play_reply. The sink runs on
// the link task and blocks until playback finishes; that blocking is
// what paces the SPEAKING face state. The buffer is PSRAM owned by the
// link and valid only for the duration of the call. No sink or no key:
// replies stay text, exactly as before.
using SpeechSinkFn = bool (*)(const uint8_t* mp3, size_t len, void* ctx);
void muse_link_set_speech_sink(SpeechSinkFn fn, void* ctx);

// True while a fetched reply clip is actually playing back. The Muse
// face drives its SPEAKING state from this — the real signal, not an
// inference from agent status codes.
bool muse_link_speaking();

// Starts WiFi + SNTP + the Muse link task. Idempotent. Safe to call when
// unconfigured (everything stays off; muse_link_state() == kOff).
void net_start();

}  // namespace aiwatchos_net
