// Pure wire-format helpers — see net_proto.hpp. Base64 and the WAV header
// are byte-compatible with muse-gadget-206's muse_hatch_base64.c / chat
// session code (same author ecosystem; formats verified against it).
#include "aiwatchos_net/net_proto.hpp"

#include <cstring>

namespace aiwatchos_net {
namespace proto {
namespace {

const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool hex_value(char c, int* v) {
    if (c >= '0' && c <= '9') { *v = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { *v = c - 'a' + 10; return true; }
    if (c >= 'A' && c <= 'F') { *v = c - 'A' + 10; return true; }
    return false;
}

std::string form_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%' && i + 2 < in.size()) {
            int hi = 0, lo = 0;
            if (hex_value(in[i + 1], &hi) && hex_value(in[i + 2], &lo)) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(c == '+' ? ' ' : c);
    }
    return out;
}

void put_u32le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}
void put_u16le(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

}  // namespace

size_t b64_len(size_t n) { return 4 * ((n + 2) / 3); }

size_t base64_encode(const uint8_t* in, size_t n, char* out) {
    size_t w = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = static_cast<uint32_t>(in[i]) << 16;
        int rem = static_cast<int>(n - i);
        if (rem > 1) v |= static_cast<uint32_t>(in[i + 1]) << 8;
        if (rem > 2) v |= in[i + 2];
        out[w++] = kB64[(v >> 18) & 63];
        out[w++] = kB64[(v >> 12) & 63];
        out[w++] = rem > 1 ? kB64[(v >> 6) & 63] : '=';
        out[w++] = rem > 2 ? kB64[v & 63] : '=';
    }
    out[w] = 0;
    return w;
}

void wav_header(uint8_t out[kWavHeaderBytes], uint32_t rate) {
    // Mirrors muse_chat_session's write_wav_header with unknown sizes.
    memcpy(out, "RIFF", 4);
    put_u32le(out + 4, 0xFFFFFFFFu);
    memcpy(out + 8, "WAVEfmt ", 8);
    put_u32le(out + 16, 16);
    put_u16le(out + 20, 1);                        // PCM
    put_u16le(out + 22, 1);                        // mono
    put_u32le(out + 24, rate);
    put_u32le(out + 28, rate * 2);                 // byte rate
    put_u16le(out + 32, 2);                        // block align
    put_u16le(out + 34, 16);                       // bits/sample
    memcpy(out + 36, "data", 4);
    put_u32le(out + 40, 0xFFFFFFFFu);
}

std::string query_escape(const std::string& in) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '.' || c == '~' || c == '!' || c == '*' ||
                    c == '\'' || c == '(' || c == ')';
        if (keep) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

std::string host_from_ws_url(const std::string& url) {
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) return "";
    size_t begin = scheme + 3;
    size_t end = url.find_first_of("/:?#", begin);
    if (end == std::string::npos) end = url.size();
    if (end <= begin) return "";
    return url.substr(begin, end - begin);
}

std::string derive_host(const std::string& explicit_host,
                        const std::string& ws_url, const std::string& vm_id,
                        const std::string& domain) {
    if (!explicit_host.empty()) return explicit_host;
    std::string from_url = host_from_ws_url(ws_url);
    if (!from_url.empty()) return from_url;
    if (!vm_id.empty() && !domain.empty()) return vm_id + "." + domain;
    return "";
}

bool parse_setup_string(const char* text, SetupFields* out) {
    if (!text || !out) return false;
    static const char kPrefix[] = "aiwatchos://setup";
    const size_t plen = sizeof(kPrefix) - 1;
    if (strncmp(text, kPrefix, plen) != 0) return false;
    const char* q = text + plen;
    if (*q == '?') ++q;
    else if (*q != 0) return false;

    *out = SetupFields{};
    std::string query(q);
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string pair = query.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string key = pair.substr(0, eq);
            std::string val = form_decode(pair.substr(eq + 1));
            if (key == "ssid") { out->ssid = val; out->has_ssid = true; }
            else if (key == "pass") { out->pass = val; out->has_pass = true; }
            else if (key == "token") { out->token = val; out->has_token = true; }
            else if (key == "vmtoken") { out->vm_token = val; out->has_vm_token = true; }
            else if (key == "vm") { out->vm = val; out->has_vm = true; }
            else if (key == "host") { out->host = val; out->has_host = true; }
            else if (key == "domain") { out->domain = val; out->has_domain = true; }
        }
        if (amp == query.size()) break;
        pos = amp + 1;
    }
    return true;
}

}  // namespace proto
}  // namespace aiwatchos_net
