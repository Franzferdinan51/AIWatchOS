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

// --- Provisioning seam -----------------------------------------------------
// The settings/BLE/serial setup flows all funnel into one string:
//   aiwatchos://setup?ssid=<..>&pass=<..>&token=<..>&vmtoken=<..>
//                     &vm=<..>&host=<..>&domain=<..>
// Values are form-encoded (%XX, '+' = space). Every field is optional;
// empty means "leave unchanged" when applied over existing config.
struct SetupFields {
    std::string ssid, pass, token, vm_token, vm, host, domain;
    bool has_ssid = false, has_pass = false, has_token = false,
         has_vm_token = false, has_vm = false, has_host = false,
         has_domain = false;
};

// Parses a setup string. Returns false when the scheme/prefix is wrong.
bool parse_setup_string(const char* text, SetupFields* out);

}  // namespace proto
}  // namespace aiwatchos_net
