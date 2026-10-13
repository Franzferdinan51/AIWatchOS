// Pure wire-format helpers for the Muse link, shared by the firmware and
// the host tests. No ESP-IDF includes: everything here is plain C++17 so
// test/net_logic_test.cpp can build it with a host g++.
//
// The formats are the muse-gadget-206 / hatch chat protocol formats, ported
// from esp32/components/muse/muse_chat_text.c and muse_chat_priv.h:
//   - a voice note is a POST /chat/stream JSON body: NOTE_HEAD, a base64
//     WAV (streaming header, unknown sizes), NOTE_TAIL;
//   - the WAV/PCM is staged so each base64 part fills one 8192-byte body
//     chunk (6144 PCM bytes -> 8192 base64 chars);
//   - the /v1/noise upgrade query escapes RFC 3986 unreserved + !~*'().
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aiwatchos_net {
namespace proto {

// --- Voice-note framing (must match the server's expectation byte-for-byte)
inline constexpr const char* kNoteHead =
    "{\"message\":\"\",\"output_modality\":\"text\",\"items\":[{\"type\":"
    "\"file\",\"mime_type\":\"audio/wav\",\"filename\":\"voice_note.wav\","
    "\"data_base64\":\"";
inline constexpr const char* kNoteTail = "\"}]}";
inline constexpr size_t kWavHeaderBytes = 44;
inline constexpr size_t kBodyChunkBytes = 8192;             // one BodyChunk
inline constexpr size_t kNotePartPcmBytes = kBodyChunkBytes / 4 * 3;  // 6144

// Standard base64 with padding (muse_hatch_base64 semantics). Returns the
// number of chars written to `out` (caller guarantees b64_len(n) room).
size_t b64_len(size_t n);
size_t base64_encode(const uint8_t* in, size_t n, char* out);

// 44-byte RIFF/WAVE header for 16-bit mono PCM at `rate`, with the RIFF and
// data sizes set to UINT32_MAX ("streaming, unknown length" — the server
// reads to the end of the body, exactly as the gadget sends it).
void wav_header(uint8_t out[kWavHeaderBytes], uint32_t rate);

// Query escaping for the /v1/noise?vm_id=... upgrade request: keeps RFC 3986
// unreserved characters plus !~*'(), percent-encodes everything else.
std::string query_escape(const std::string& in);

// Host (authority, no port) of a ws/wss URL, e.g.
// "wss://abc123.hatch.example/" -> "abc123.hatch.example". "" if unparseable.
std::string host_from_ws_url(const std::string& url);

// Which host the TLS connection opens, in priority order:
//   1. explicit_host (settings override)
//   2. host parsed from the account API's vm_ws_url
//   3. "<vm_id>.<domain>" when both are known
//   4. "" (unresolvable — caller must treat the link as unconfigured)
std::string derive_host(const std::string& explicit_host,
                        const std::string& ws_url, const std::string& vm_id,
                        const std::string& domain);

// --- Agent / task status (chat subscribe events) ---------------------------
// The Hatch stream emits agent.status / task.status events whose payload
// carries an activity_code (the companion's chat.dart falls back to
// `status` when it is absent) and, for tasks, sometimes a human label.
// The reference gadget firmware reduces the pair to a busy flag
// (muse_chat_session.cpp: code present -> busy unless "online"/"idle";
// status only -> busy unless "completed"/"failed"). That combine step
// lives here so the semantics are host-testable; the firmware extracts
// the fields with cJSON and calls this.
struct AgentStatus {
    bool present = false;   // the event carried a usable code
    bool busy = false;
    char code[40] = {};     // activity_code, else status (raw, capped)
    char label[96] = {};    // task label when the payload carried one
};

// activity_code / status / label are the extracted payload fields
// (nullptr or "" = field absent or not a string — both count as absent,
// matching the companion's _text()). Returns false (and zeroes `out`)
// when neither activity_code nor status is usable.
bool agent_status_combine(const char* activity_code, const char* status,
                          const char* label, AgentStatus* out);

// --- Provisioning seam -----------------------------------------------------
// The settings/BLE/serial setup flows all funnel into one string:
//   aiwatchos://setup?ssid=<..>&pass=<..>&token=<..>&vmtoken=<..>
//                     &vm=<..>&host=<..>&domain=<..>
//                     &ttsurl=<..>&ttskey=<..>&ttsmodel=<..>&ttsvoice=<..>
// Values are form-encoded (%XX, '+' = space). Every field is optional;
// empty means "leave unchanged" when applied over existing config.
struct SetupFields {
    std::string ssid, pass, token, vm_token, vm, host, domain;
    std::string tts_base_url, tts_api_key, tts_model, tts_voice;
    bool has_ssid = false, has_pass = false, has_token = false,
         has_vm_token = false, has_vm = false, has_host = false,
         has_domain = false;
    bool has_tts_base_url = false, has_tts_api_key = false,
         has_tts_model = false, has_tts_voice = false;
};

// Parses a setup string. Returns false when the scheme/prefix is wrong.
bool parse_setup_string(const char* text, SetupFields* out);

// --- Spoken replies (TTS) ---------------------------------------------------
// Replies are spoken by an external OpenAI-shaped speech endpoint
// (POST {base}/v1/audio/speech -> MP3) — see docs/tts-investigation.md
// for why no speech source exists behind the Hatch token. Everything
// format-shaped about the port lives here so it is host-testable; the
// HTTP fetch and playback live in muse_tts.cpp (device only). No
// provider is baked in: base URL, model, voice and key all come from
// config (see net.hpp NetConfig), and an empty key disables the feature.

// Hard cap on one fetched MP3 — the reference gadget's MP3_BUF scale.
// A response that grows past this aborts the fetch; the reply stays text.
inline constexpr size_t kSpeechMp3CapBytes = 512 * 1024;
// Per-request input cap (code points) and per-reply chunk budget:
// longer replies speak their first chunks; the rest stays text-only
// (the caption always shows the full reply).
inline constexpr size_t kSpeechChunkChars = 1000;
inline constexpr size_t kSpeechMaxChunks = 3;

// The text that gets spoken: the companion caption cleaner's rules
// (apps/muse/include/muse_captions.hpp, captions clean()) — fenced
// code -> one space, markdown links -> their text, markdown chars
// removed, whitespace collapsed, trimmed — WITHOUT the caption's
// 240-char clip: speech wants the whole reply, chunked by
// speech_chunks() instead. A deliberate mirror, kept here for the same
// reason muse_link.cpp keeps a local ADPCM copy: the component graph
// stays acyclic (apps/muse depends on this component, not vice versa).
// The two implementations share test vectors.
std::string speech_clean(const std::string& text);

// Splits cleaned text into speakable chunks of at most max_chars code
// points each, preferring sentence boundaries (. ! ? U+2026), then
// word boundaries, then a hard code-point cut (never mid-codepoint).
// At most max_chunks chunks are returned; any remainder is dropped.
std::vector<std::string> speech_chunks(const std::string& cleaned,
                                       size_t max_chars,
                                       size_t max_chunks);

// "{base}/v1/audio/speech" with any trailing '/' on base stripped.
// "" when base is empty.
std::string speech_url(const std::string& base_url);

// The request body: {"model":..,"voice":..,"input":..,
// "response_format":"mp3"} with JSON string escaping.
std::string speech_body(const std::string& model, const std::string& voice,
                        const std::string& input);

// Pipeline state for one spoken reply, driven by muse_tts.cpp and
// unit-tested here. begin(n) starts fetching chunk 1; fetch_ok() moves
// to playing; play_done() loops back to fetching while chunks remain,
// else finishes idle; any failure lands in kFailed, from which begin()
// may start a fresh reply. Events that don't apply to the current
// state are no-ops. speaking() is the face signal: true only while
// audio is actually playing.
enum class SpeechState : uint8_t { kIdle, kFetching, kPlaying, kFailed };
struct SpeechMachine {
    SpeechState state = SpeechState::kIdle;
    size_t chunks_total = 0;
    size_t chunks_done = 0;   // chunks fully played
    void begin(size_t chunks);
    void fetch_ok();
    void fetch_failed();
    void play_done();
    void play_failed();
    void reset();
    bool speaking() const { return state == SpeechState::kPlaying; }
    bool active() const {
        return state == SpeechState::kFetching ||
               state == SpeechState::kPlaying;
    }
};

}  // namespace proto
}  // namespace aiwatchos_net
