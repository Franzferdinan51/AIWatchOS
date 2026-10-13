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
//   2. domain — the Muse edge host (default hatch.metaaivm.com); it
//      routes by the ?vm_id= upgrade query, as in every official SDK
//      client. A per-VM subdomain is never constructed.
//   3. host parsed from the vm_ws_url of the account API (only
//      reachable when the domain is explicitly cleared)
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

// --- Heard transcript (chat subscribe message.user, M5) --------------------
// The server patches the voice-note message with what STT heard: a
// message.user event whose text reads "<transcript>\n[file:audio/wav ...]"
// (attachment suffix added by the server), or exactly "[Voice note]"
// before transcription lands. The reference gadget firmware
// (muse_chat_link.c on_row) cuts the suffix at "\n[file:", ignores the
// placeholder, and only then surfaces the heard text; the companion's
// _applyHeard does the same replacement on its "Voice note" bubble.
// Cleans `raw` into `out` (NUL-terminated, capped at cap-1 chars on a
// UTF-8 boundary). Returns false — and writes "" — when nothing usable
// remains (null/empty input, whitespace only, or the placeholder).
bool heard_transcript_clean(const char* raw, char* out, size_t cap);

// --- Provisioning seam -----------------------------------------------------
// The settings/BLE/serial setup flows all funnel into one string:
//   aiwatchos://setup?ssid=<..>&pass=<..>&token=<..>&vmtoken=<..>
//                     &vm=<..>&host=<..>&domain=<..>
// Values are form-encoded (%XX, '+' = space). Every field is optional;
// empty means "leave unchanged" when applied over existing config.
struct SetupFields {
    std::string ssid, pass, token, vm_token, vm, host, domain;
    // M10 TTS provisioning: ttsurl / ttskey / ttsmodel / ttsvoice carry
    // the speech endpoint, API key, model, and voice; speak is the
    // initial Speak-replies toggle ("1"/"true" = on, "0" = off).
    std::string tts_url, tts_key, tts_model, tts_voice;
    bool tts_speak = false;
    bool has_tts_url = false, has_tts_key = false, has_tts_model = false,
         has_tts_voice = false, has_tts_speak = false;
    bool has_ssid = false, has_pass = false, has_token = false,
         has_vm_token = false, has_vm = false, has_host = false,
         has_domain = false;
};

// Parses a setup string. Returns false when the scheme/prefix is wrong.
bool parse_setup_string(const char* text, SetupFields* out);

// --- Cloud TTS request shaping (M10) ---------------------------------------
// The companion's openrouter_tts.dart contract, ported pure so it is
// host-testable: POST the endpoint with {"model","input",
// "response_format":"mp3"} (plus "voice" only when set), the reply
// text split into <=1800-char chunks on sentence boundaries, played
// back to back. One deliberate divergence: a single sentence longer
// than the cap is hard-split at the cap (on a UTF-8 boundary) so no
// request ever exceeds it — the companion would send it whole.
inline constexpr size_t kTtsChunkChars = 1800;
std::vector<std::string> tts_chunks(const std::string& text,
                                    size_t max_chars = kTtsChunkChars);
// The exact JSON request body for one chunk (keys in the companion's
// order; voice omitted when empty).
std::string tts_request_json(const std::string& model,
                             const std::string& voice,
                             const std::string& input);

}  // namespace proto
}  // namespace aiwatchos_net
