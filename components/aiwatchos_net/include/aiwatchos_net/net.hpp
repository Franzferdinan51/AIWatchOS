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

    // Cloud TTS (M10): an OpenAI-shaped POST /v1/audio/speech endpoint
    // (the companion's OpenRouter path). Everything is provisioning —
    // no endpoint, model, or key is compiled in as a requirement, and
    // an empty key means speech is off (text replies unaffected).
    std::string tts_url;        // full speech endpoint URL
    std::string tts_key;        // API key (Bearer) — never logged/rendered
    std::string tts_model;      // model id sent in the request body
    std::string tts_voice;      // voice id ("" = provider default)
    bool tts_speak = false;     // the OS Settings "Speak replies" toggle

    // Hermes hub away-from-home endpoint: the hub host's Tailscale
    // Funnel (wss). The primary home-LAN endpoint stays in the Hermes
    // app's own Kconfig (HERMES_HOST/HERMES_PORT); the remote endpoint
    // is provisioning-shaped like the TTS fields above because the
    // funnel name is deployment data, not app behavior. The Hermes
    // link layer reads these through net_config() on every reconnect
    // attempt, so provisioning takes effect without a reflash.
    std::string hermes_remote_host;    // funnel host ("" = no remote)
    int hermes_remote_port = 8443;
    bool hermes_remote_enabled = true;

    // True when the remote Hermes endpoint may be attempted.
    bool hermes_remote_configured() const {
        return hermes_remote_enabled && !hermes_remote_host.empty();
    }

    // True when speech could work if enabled: endpoint + key present.
    bool tts_provisioned() const {
        return !tts_url.empty() && !tts_key.empty();
    }

    // True when a Muse session is even attemptable: WiFi credentials and
    // either an account token or a direct VM token.
    bool muse_configured() const {
        return !wifi_ssid.empty() &&
               (!hatch_token.empty() || !vm_token.empty());
    }
    bool wifi_configured() const { return !wifi_ssid.empty(); }
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

// The latest heard transcript (M5): what STT heard for the most recent
// voice note, retained from the subscribe stream's message.user event
// and cleaned (attachment suffix cut, "[Voice note]" placeholder
// suppressed) by proto::heard_transcript_clean. Thread-safe snapshot
// copy; "" when no transcript has arrived this session.
bool muse_link_last_transcript(char* out, size_t cap);

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

// --- Spoken replies (M10, cloud TTS) ---------------------------------------
// The watch-side client for the companion's opt-in OpenRouter speech
// path, generalized to any OpenAI-shaped endpoint (see NetConfig's
// tts_* fields). The engine (muse_tts.cpp) chunks the cleaned reply,
// POSTs each chunk, and plays the MP3 through the registered player.
// Speech is an enhancement only: off, unprovisioned, or failed speech
// never affects the text reply.

// The persisted OS Settings "Speak replies" toggle.
bool muse_tts_speak_enabled();
void muse_tts_set_speak_enabled(bool on);
// Endpoint + key present (provisioning state for status pages).
bool muse_tts_provisioned();
// A fetch or playback is in flight.
bool muse_tts_speaking();
// Barge-in: drop pending speech and remaining chunks. (The in-flight
// MP3 itself is aborted by the app's player — muse_speech_stop.)
void muse_tts_stop();
// Hand a CLEANED reply (companion caption cleaning, full text — the
// engine chunks it) to the speech engine. Latest wins: unspeaked
// older text is replaced. No-op unless enabled + provisioned + a
// player is registered.
void muse_tts_speak_text(const char* cleaned_text);
// Playback seam: the Muse app registers its MP3 player so this
// component never depends on the app layer (the outbox pump pattern).
// The callback blocks until playback ends or the app aborts it, and
// returns the number of 16 kHz frames written.
using SpeechPlayerFn = size_t (*)(const uint8_t* mp3, size_t len, void* ctx);
void muse_tts_set_player(SpeechPlayerFn fn, void* ctx);

// Starts WiFi + SNTP + the Muse link task. Idempotent. Safe to call when
// unconfigured (everything stays off; muse_link_state() == kOff).
void net_start();

}  // namespace aiwatchos_net
