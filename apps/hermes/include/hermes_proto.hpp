// Hermes Gadget Protocol v1 — portable device-side core for AIWatchOS.
//
// Pure C++ (no ESP-IDF, no heap-heavy containers): the whole client state
// machine lives here so it runs identically on the watch and in host tests.
// The platform supplies three things through HermesSinks: a WebSocket
// transport, the audio/flash backends, and a random source. Everything else
// — identity, HMAC auth, pairing, conversation, heartbeat, OTA — is here.
//
// Protocol reference: HermesGadget docs/protocol.md. The identity/auth
// vectors are pinned to the same published vectors the HermesGadget server
// and firmware test suites use (see tests in app_registry_test.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace aiwatchos_hermes {

// ---------------------------------------------------------------------------
// Crypto primitives (portable SHA-256 / HMAC-SHA256 / base64 / hex).
// Verified against the HermesGadget published vectors:
//   key = 00..1f -> device_id "hg-630dcd2966c43366"
//   auth mac for nonce "bm9uY2Utbm9uY2Utbm9uY2U=" ->
//   "AMUEF53Phk8+1vHw7R8PgiDwlPd8rXUx+cgyYv1YJX0="
// ---------------------------------------------------------------------------

void hermes_sha256(const uint8_t* data, size_t len, uint8_t out[32]);
void hermes_sha256_hex(const uint8_t* data, size_t len, char out[65]);
std::string hermes_base64_encode(const uint8_t* data, size_t len);
// Strict-ish base64 decode (padding optional). Returns false on bad input
// or when the decoded bytes would exceed out_cap.
bool hermes_base64_decode(const char* in, uint8_t* out, size_t out_cap, size_t* out_len);

// Streaming SHA-256 (OTA image verification). Storage is opaque and fixed.
struct HermesSha256 {
    uint32_t h[8];
    uint8_t block[64];
    size_t block_len;
    uint64_t total_len;
    void init();
    void update(const uint8_t* data, size_t len);
    void finish(uint8_t out[32]);
    void finish_hex(char out[65]);
};

// "hg-" + first 16 hex chars of SHA-256(key).
std::string hermes_device_id_for_key(const uint8_t key[32]);
// Base64 HMAC-SHA256 over "hermes-gadget/v1|" + device_id + "|" + nonce.
std::string hermes_auth_mac_b64(const uint8_t key[32], const char* device_id, const char* nonce_b64);
// Base64 HMAC-SHA256 over "hermes-gadget/v1|ota|" + device_id + "|" + nonce
// + "|" + sha256_hex + "|" + size (decimal).
std::string hermes_ota_mac_b64(const uint8_t key[32], const char* device_id, const char* nonce_b64,
                               const char* sha256_hex, uint32_t size);

// ---------------------------------------------------------------------------
// Binary frames: [u8 channel][u8 stream][u16 LE seq][payload]
//   0x01 audio PCM16 LE, 0x02 image RGB565 LE, 0x03 firmware
// ---------------------------------------------------------------------------

constexpr uint8_t kHermesChannelAudio = 0x01;
constexpr uint8_t kHermesChannelImage = 0x02;
constexpr uint8_t kHermesChannelFirmware = 0x03;
constexpr size_t kHermesBinaryHeader = 4;
constexpr uint32_t kHermesOtaMaxSize = 2031616;   // caps.ota.max_size we advertise

size_t hermes_binary_header_pack(uint8_t* dst, uint8_t channel, uint8_t stream, uint16_t seq);
bool hermes_binary_parse(const uint8_t* data, size_t len, uint8_t* channel, uint8_t* stream,
                         uint16_t* seq, const uint8_t** payload, size_t* payload_len);

// ---------------------------------------------------------------------------
// WebSocket message reassembly (pure: shared by the link layer and host
// tests). Fragmented WS messages accumulate in the caller's buffer until
// the final fragment; a message that would exceed max_len is dropped
// WHOLE — later fragments of the same message are ignored until it ends —
// so a corrupt truncation is never delivered as a valid message.
// ---------------------------------------------------------------------------

struct HermesReasmState {
    size_t len = 0;      // bytes accumulated for the in-progress message
    uint8_t opcode = 0;  // first fragment's opcode (0x1 text, 0x2 binary)
    bool drop = false;   // set: the in-progress message is oversized, ignore it
};

enum class HermesReasmOut : uint8_t {
    Ignored,    // do not copy this fragment, nothing is ready
    Buffered,   // copy frag_len bytes at *copy_at, message still in progress
    Complete,   // copy frag_len bytes at *copy_at, then emit *emit_len bytes
};

// Feed one WS fragment: op is the frame opcode (0x0 = continuation),
// frag_off/frag_len locate it in the message, msg_len is the full message
// length, fin is the frame FIN bit. max_len bounds one message.
HermesReasmOut hermes_reasm_feed(HermesReasmState* st, uint8_t op, size_t frag_off,
                                 size_t frag_len, size_t msg_len, bool fin,
                                 size_t max_len, size_t* copy_at, size_t* emit_len);

// ---------------------------------------------------------------------------
// Hub endpoint failover (away-from-home).
//
// The hub is reachable two ways: the home-LAN endpoint (ws://, the
// HERMES_HOST/HERMES_PORT Kconfig pair in apps/hermes) and a remote
// endpoint (wss:// through the hub host's Tailscale Funnel, provisioned
// via the shared net config — see NetConfig::hermes_remote_*). This pure
// machine decides which endpoint each connection attempt uses; the link
// layer feeds it attempt outcomes and owns the backoff timing, so the
// policy is fully host-testable:
//
//   - every fresh cycle starts on LAN: a successful connect and a
//     dropped established link both re-arm LAN first, so returning
//     home recovers the LAN endpoint automatically;
//   - kHermesLanFailoverThreshold consecutive failed LAN attempts move
//     the next attempt to REMOTE (only when a remote endpoint is
//     configured + enabled — otherwise LAN is retried forever);
//   - a failed REMOTE attempt hands the cycle back to LAN with the
//     streak reset, so the endpoints alternate L,L,R,L,L,R... while
//     both are down and the link layer's backoff ladder paces them.
// ---------------------------------------------------------------------------

enum class HermesEndpoint : uint8_t { Lan = 0, Remote = 1 };

constexpr int kHermesLanFailoverThreshold = 2;

const char* hermes_endpoint_name(HermesEndpoint ep);   // "LAN" / "REMOTE"

struct HermesFailover {
    bool remote_enabled = false;   // remote configured + enabled (link layer syncs per attempt)
    HermesEndpoint next = HermesEndpoint::Lan;     // endpoint the next attempt uses
    HermesEndpoint active = HermesEndpoint::Lan;   // endpoint of the last successful connect
    bool has_active = false;                       // any successful connect yet
    int lan_failures = 0;                          // consecutive failed LAN attempts

    HermesEndpoint next_endpoint() const { return next; }

    void on_connected(HermesEndpoint ep) {
        active = ep;
        has_active = true;
        lan_failures = 0;
        next = HermesEndpoint::Lan;   // fresh cycles prefer LAN
    }
    void on_attempt_failed(HermesEndpoint ep) {
        if (ep == HermesEndpoint::Remote) {
            lan_failures = 0;
            next = HermesEndpoint::Lan;
            return;
        }
        ++lan_failures;
        next = (remote_enabled && lan_failures >= kHermesLanFailoverThreshold)
                   ? HermesEndpoint::Remote
                   : HermesEndpoint::Lan;
    }
    void on_link_down() {   // an established link dropped: fresh cycle
        lan_failures = 0;
        next = HermesEndpoint::Lan;
    }
};

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

enum class HermesLinkState : uint8_t {
    Offline = 0,   // no transport (or dead and not yet restarted)
    Connecting,    // transport up, handshake in flight
    Pairing,       // session up, waiting for the owner to approve pairing
    Linked,        // session up and paired — full conversation available
    AuthFailed,    // server rejected our key; owner must forget + re-enroll
};

// One completed exchange for the H1 history ring: the transcript the
// hub heard plus the reply it produced, stamped with the client clock
// at turn end (epoch-based ms on device; the adapter renders HH:MM).
struct HermesExchange {
    char transcript[512] = {};
    char reply[4096] = {};
    uint64_t end_ms = 0;
};

// Everything the renderer needs, in fixed buffers (no per-frame allocation).
// The owning HermesClient is heap-allocated once (PSRAM on device).
struct HermesView {
    // H1: completed exchanges, oldest first; the live pair below is the
    // in-progress / most recent exchange. Session-scoped: cleared by
    // session.new, never persisted (the pair is the persisted surface).
    static constexpr int kHermesHistoryMax = 6;
    HermesExchange history[kHermesHistoryMax];
    uint8_t history_count = 0;
    HermesLinkState link = HermesLinkState::Offline;
    bool paired = false;
    uint8_t queued = 0;            // offline outbox depth (buddy-harvest)
    char server[24] = {};          // welcome "server" (hub: "hermes")
    char session[40] = {};         // welcome "session" id
    char pairing_code[16] = {};    // 8-char code while Pairing
    char pairing_cmd[96] = {};     // full approve command from the server
    char status[160] = {};         // live working-state phrase ("" = idle)
    char transcript[512] = {};     // what speech-to-text heard
    char reply[4096] = {};         // latest reply text (cumulative, capped)
    bool busy = false;             // a turn is in progress
    char notice[160] = {};         // transient one-liner
    uint64_t notice_until_ms = 0;
    // Yes/no prompt from Hermes (answer exactly once, or let it expire).
    bool prompt_active = false;
    char prompt_id[32] = {};
    char prompt_title[96] = {};
    char prompt_text[512] = {};
    uint64_t prompt_since_ms = 0;
    uint32_t prompt_ttl_s = 0;     // 0 = no expiry
    // display{} card.
    bool card_active = false;
    char card_title[96] = {};
    char card_body[1024] = {};
    uint64_t card_until_ms = 0;    // 0 = sticky until dismissed
    // image{} card (RGB565 LE pixels, heap buffer owned by the client).
    bool image_active = false;
    int image_w = 0;
    int image_h = 0;
    uint64_t image_until_ms = 0;
    // OTA progress for the UI.
    bool ota_active = false;
    uint32_t ota_received = 0;
    uint32_t ota_size = 0;
    char error[96] = {};           // last protocol error (auth failures etc.)
};

// Platform backends. All callbacks may be nullptr except send_text /
// send_binary; audio and OTA features degrade gracefully without theirs.
struct HermesSinks {
    bool (*send_text)(void* ctx, const char* json, size_t len) = nullptr;
    bool (*send_binary)(void* ctx, const uint8_t* data, size_t len) = nullptr;
    void (*audio_begin)(void* ctx, uint32_t rate) = nullptr;
    void (*audio_write)(void* ctx, const int16_t* samples, size_t count) = nullptr;
    void (*audio_end)(void* ctx) = nullptr;
    void (*audio_abort)(void* ctx) = nullptr;
    bool (*ota_begin)(void* ctx, uint32_t size) = nullptr;          // prepare inactive slot
    bool (*ota_write)(void* ctx, const uint8_t* data, size_t len) = nullptr;
    bool (*ota_finish)(void* ctx) = nullptr;                        // validate + set boot
    void (*ota_abort)(void* ctx) = nullptr;
    void (*fill_random)(void* ctx, uint8_t* out, size_t len) = nullptr;
    // Big-buffer allocator for the image pixel buffer (>4 KB rule: on
    // device this is heap_caps MALLOC_CAP_SPIRAM, fail-closed — a nullptr
    // result drops the image, never falls back to internal RAM).
    void* (*alloc_big)(void* ctx, size_t len) = nullptr;
    void (*free_big)(void* ctx, void* ptr) = nullptr;
    void* ctx = nullptr;
};

struct HermesConfig {
    char name[40] = "AIWatchOS";
    char board[40] = "esp32s3-touch-amoled-2.06";
    char firmware[16] = "1.0.0";
    int display_w = 410;
    int display_h = 502;
    int image_max_w = 410;
    int image_max_h = 300;
};

class HermesClient {
 public:
    // Handshake watchdog: a transport that never delivers welcome within
    // this long after connect is cycled (Offline + reconnect + a link
    // recycle request) instead of hanging on CONNECTING forever. Generous
    // on purpose — one default heartbeat interval; a healthy hub answers
    // in well under a second.
    static constexpr uint32_t kWelcomeTimeoutMs = 20000;
    // Image watchdog: an image transfer stalled this long is abandoned
    // with a user-visible notice (mirrors the 30 s OTA stall rule).
    static constexpr uint32_t kImageStallMs = 30000;

    // sinks/config are copied. Call set_key() before on_transport_open().
    void init(const HermesSinks& sinks, const HermesConfig& cfg);
    void set_key(const uint8_t key[32]);
    const char* device_id() const { return device_id_; }
    bool has_key() const { return has_key_; }

    // Transport events (link layer calls these).
    void on_transport_open(uint64_t now_ms);
    void on_transport_closed(uint64_t now_ms);
    void on_text(const char* json, size_t len, uint64_t now_ms);
    void on_binary(const uint8_t* data, size_t len, uint64_t now_ms);
    // Periodic drive: heartbeat dead-detection, TTL expiry, state updates,
    // OTA timeouts. Cheap; call at frame rate or from the link task.
    void tick(uint64_t now_ms);

    // User intents (UI calls these).
    bool send_chat_text(const char* text);   // sends now, or queues 1 while unpaired/connecting
    bool answer_prompt(bool yes);            // false when no prompt is active
    void dismiss_card();
    // session.new: ask the hub for a fresh agent session (hub injects
    // /new). Reference-firmware semantics (start_new_session): gated on
    // paired+welcomed, stops capture/playback, clears prompt/card and
    // the local transcript, then sends. No ack frame exists — the next
    // turn belongs to the new session.
    bool new_session(uint64_t now_ms);
    void cancel(uint64_t now_ms);            // cancel capture, else cancel the running turn
    bool talk_start(uint64_t now_ms);
    void talk_feed(const int16_t* pcm, size_t frames);
    void talk_stop(uint64_t now_ms);
    void talk_cancel();                      // discard the in-progress utterance
    bool talk_active() const { return capturing_; }
    // pct -1 = unknown; charging -1 = unknown, 0 = not charging, 1 = charging.
    void set_battery(int pct, int charging, uint64_t now_ms);

    // Forget the rejected identity after an auth failure (companion of
    // set_key() with a fresh key, driven by the adapter's forget flow):
    // clears the AuthFailed latch so the link may reconnect and re-enroll.
    void forget();
    // Restore the persisted conversation pair at boot (buddy-harvest:
    // chat persistence, watch scale — the transcript+reply pair IS the
    // watch's conversation state).
    void restore_conversation(const char* transcript, const char* reply);
    // Offline outbox introspection for the persistence layer.
    int outbox_count() const { return outbox_count_; }
    const char* outbox_at(int i) const;   // i = 0 is the oldest queued text
    uint32_t outbox_version() const { return outbox_version_; }

    HermesLinkState state() const { return view_.link; }
    bool paired() const { return view_.paired; }
    bool wants_reconnect() const { return wants_reconnect_; }
    // The handshake watchdog asked the link layer to tear down and redial
    // the live socket (consume-on-read: the link task takes it once).
    bool take_cycle_request() {
        bool r = want_cycle_;
        want_cycle_ = false;
        return r;
    }
    bool ota_reboot_pending() const { return ota_reboot_pending_; }
    void ota_reboot_consumed() { ota_reboot_pending_ = false; }
    const HermesView& view() const { return view_; }
    // RGB565 LE pixels for view().image_w x image_h (nullptr when none).
    const uint8_t* image_pixels() const { return img_buf_; }

 private:
    void send_json(void* root);   // a cJSON* (kept opaque: no cJSON in this header)
    void send_state(uint64_t now_ms);
    void send_text_frame(const char* text);
    void flush_pending_text(uint64_t now_ms);
    // H1: move the current transcript+reply pair into the history ring
    // (called when a new exchange begins; no-op when the pair is empty
    // or was already archived).
    void archive_pair();
    void set_link(HermesLinkState s);
    void show_notice(const char* text, uint32_t ttl_s, uint64_t now_ms);
    void ota_fail(const char* code, const char* message, uint64_t now_ms);
    void ota_reset();

    HermesSinks sinks_{};
    HermesConfig cfg_{};
    HermesView view_{};

    uint8_t key_[32] = {};
    bool has_key_ = false;
    char device_id_[20] = {};               // "hg-" + 16 hex + NUL

    bool transport_up_ = false;
    bool welcomed_ = false;
    uint32_t heartbeat_s_ = 20;
    uint64_t last_rx_ms_ = 0;
    uint64_t connect_ms_ = 0;              // on_transport_open stamp (welcome watchdog)
    bool wants_reconnect_ = false;
    bool want_cycle_ = false;              // handshake watchdog: link must recycle the socket

    int battery_pct_ = -1;
    int battery_sent_ = -2;                  // last value reported via state{}
    int charging_ = -1;                      // -1 unknown, 0 no, 1 yes
    int charging_sent_ = -2;                 // last value reported via state{}
    uint64_t last_state_ms_ = 0;

    // H1 archival state: pair_end_ms_ stamps the last turn.end;
    // pair_archived_ suppresses double-archival of the same pair.
    uint64_t pair_end_ms_ = 0;
    bool pair_archived_ = false;

    unsigned msg_counter_ = 0;
    // Offline outbox (buddy-harvest): bounded FIFO, flushed in order once
    // paired. Depth is watch-scale (no keyboard — sends are rare and
    // programmatic); a full queue evicts the OLDEST text. The link layer
    // persists the ring via outbox_at()/outbox_version().
    static constexpr int kOutboxMax = 4;
    char outbox_[kOutboxMax][512] = {};
    uint8_t outbox_head_ = 0;
    uint8_t outbox_count_ = 0;
    uint32_t outbox_version_ = 0;            // bumps on every queue mutation

    // Mic capture (device -> server, channel 1).
    bool capturing_ = false;
    uint8_t talk_stream_ = 0;
    uint16_t talk_seq_ = 0;
    char talk_id_[16] = {};
    uint32_t talk_samples_ = 0;
    int16_t talk_buf_[640] = {};             // one 40 ms frame at 16 kHz
    size_t talk_fill_ = 0;

    // Spoken-reply playback (server -> device, channel 1).
    int play_stream_ = -1;

    // Image receive (channel 2). Pixel buffer is heap-allocated lazily
    // (PSRAM on device) and capped at cfg image_max_w * image_max_h * 2.
    int img_stream_ = -1;
    size_t img_received_ = 0;
    size_t img_expected_ = 0;
    uint64_t img_last_ms_ = 0;             // last image.start/chunk (stall watchdog)
    uint8_t* img_buf_ = nullptr;

    // OTA receive (channel 3).
    enum class OtaState : uint8_t { Idle, Offered, Receiving } ota_state_ = OtaState::Idle;
    uint8_t ota_stream_ = 0;
    uint32_t ota_size_ = 0;
    char ota_sha_hex_[65] = {};
    char ota_version_[32] = {};
    char ota_nonce_b64_[25] = {};           // base64 of 16 bytes + NUL
    uint32_t ota_received_ = 0;
    uint32_t ota_last_ack_ = 0;
    uint16_t ota_expect_seq_ = 0;
    uint64_t ota_last_chunk_ms_ = 0;
    HermesSha256 ota_sha_{};
    bool ota_reboot_pending_ = false;
};

}  // namespace aiwatchos_hermes
