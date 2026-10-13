// Pure wire-format helpers — see net_proto.hpp. Base64 and the WAV header
// are byte-compatible with muse-gadget-206's muse_hatch_base64.c / chat
// session code (same author ecosystem; formats verified against it).
#include "aiwatchos_net/net_proto.hpp"

#include <cstdio>
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
    // The Muse edge host routes by the ?vm_id= query parameter; every
    // official client (esp32 chat session, esp32 Link, Linux link
    // client) dials that one host and treats vm_ws_url as metadata.
    // Never construct a per-VM subdomain: "<vm_id>.<domain>" is not a
    // routable service host. The configured domain IS the edge host
    // (its default is hatch.metaaivm.com).
    (void)vm_id;  // the VM is selected by the upgrade query, not the host
    if (!domain.empty()) return domain;
    // Only when the domain is explicitly cleared does the account
    // response URL get a say (a private edge named nowhere else).
    std::string from_url = host_from_ws_url(ws_url);
    if (!from_url.empty()) return from_url;
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
            else if (key == "ttsurl") { out->tts_url = val; out->has_tts_url = true; }
            else if (key == "ttskey") { out->tts_key = val; out->has_tts_key = true; }
            else if (key == "ttsmodel") { out->tts_model = val; out->has_tts_model = true; }
            else if (key == "ttsvoice") { out->tts_voice = val; out->has_tts_voice = true; }
            else if (key == "speak") {
                out->tts_speak = (val == "1" || val == "true");
                out->has_tts_speak = true;
            }
            else if (key == "hhost2") {
                out->hermes_remote_host = val;
                out->has_hermes_remote_host = true;
            }
            else if (key == "hport2") {
                out->hermes_remote_port = val;
                out->has_hermes_remote_port = true;
            }
            else if (key == "hrem") {
                out->hermes_remote_on = (val == "1" || val == "true");
                out->has_hermes_remote_on = true;
            }
        }
        if (amp == query.size()) break;
        pos = amp + 1;
    }
    return true;
}

bool parse_tcp_port(const char* s, int* out) {
    if (!s || !out || !*s) return false;
    long v = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (*p - '0');
        if (v > 65535) return false;
    }
    if (v < 1) return false;
    *out = static_cast<int>(v);
    return true;
}

bool heard_transcript_clean(const char* raw, char* out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!raw) return false;
    // Cut the server's attachment suffix: "\n[file:audio/wav ...]".
    const char* end = raw + std::strlen(raw);
    const char* att = std::strstr(raw, "\n[file:");
    if (att) end = att;
    // Trim surrounding whitespace (the heard line itself is one line).
    const char* p = raw;
    auto is_ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (p < end && is_ws(*p)) ++p;
    while (end > p && is_ws(end[-1])) --end;
    if (p >= end) return false;
    // The pre-transcription placeholder is not a transcript.
    if (static_cast<size_t>(end - p) == 12 &&
        std::strncmp(p, "[Voice note]", 12) == 0) {
        return false;
    }
    size_t n = static_cast<size_t>(end - p);
    if (n >= cap) {
        n = cap - 1;
        // Never split a UTF-8 code point at the cap.
        while (n > 0 && (static_cast<unsigned char>(p[n]) & 0xC0) == 0x80) --n;
    }
    std::memcpy(out, p, n);
    out[n] = '\0';
    return n > 0;
}

namespace {

// JSON string escaping for the TTS request body: quotes, backslash,
// and control characters; all other bytes (UTF-8 included) pass through
// raw, which JSON permits.
void json_escape_into(const std::string& in, std::string* out) {
    static const char kHex[] = "0123456789abcdef";
    for (unsigned char c : in) {
        switch (c) {
            case '"': *out += "\\\""; break;
            case '\\': *out += "\\\\"; break;
            case '\n': *out += "\\n"; break;
            case '\r': *out += "\\r"; break;
            case '\t': *out += "\\t"; break;
            default:
                if (c < 0x20) {
                    *out += "\\u00";
                    *out += kHex[c >> 4];
                    *out += kHex[c & 15];
                } else {
                    *out += static_cast<char>(c);
                }
        }
    }
}

bool tts_is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

std::string tts_trimmed(const std::string& in) {
    size_t b = 0, e = in.size();
    while (b < e && tts_is_ws(in[b])) ++b;
    while (e > b && tts_is_ws(in[e - 1])) --e;
    return in.substr(b, e - b);
}

// Largest prefix of `s` (starting at byte `pos`) of at most `max` bytes
// that ends on a UTF-8 code-point boundary.
size_t tts_utf8_span(const std::string& s, size_t pos, size_t max) {
    size_t n = max;
    if (pos + n > s.size()) n = s.size() - pos;
    while (n > 0 && pos + n < s.size() &&
           (static_cast<unsigned char>(s[pos + n]) & 0xC0) == 0x80) {
        --n;
    }
    return n ? n : max;   // degenerate bytes: take the raw span
}

}  // namespace

std::vector<std::string> tts_chunks(const std::string& text, size_t max_chars) {
    std::vector<std::string> out;
    const std::string trimmed = tts_trimmed(text);
    if (trimmed.empty() || max_chars == 0) return out;
    if (trimmed.size() <= max_chars) {
        out.push_back(trimmed);
        return out;
    }
    // Sentence spans: a boundary follows [.!?] + whitespace (the
    // companion's (?<=[.!?])\s+ split); punctuation stays with its
    // sentence, the separating whitespace is dropped.
    std::vector<std::string> sentences;
    size_t start = 0;
    for (size_t i = 0; i < trimmed.size(); ++i) {
        const char c = trimmed[i];
        if ((c == '.' || c == '!' || c == '?') && i + 1 < trimmed.size() &&
            tts_is_ws(trimmed[i + 1])) {
            sentences.push_back(trimmed.substr(start, i + 1 - start));
            size_t j = i + 1;
            while (j < trimmed.size() && tts_is_ws(trimmed[j])) ++j;
            start = j;
            i = j - 1;
        }
    }
    if (start < trimmed.size()) sentences.push_back(trimmed.substr(start));

    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    };
    for (const std::string& s : sentences) {
        if (s.size() > max_chars) {
            // Oversize single sentence: flush, then hard-split at the
            // cap (see header note — no request may exceed it).
            flush();
            size_t pos = 0;
            while (pos < s.size()) {
                const size_t n = tts_utf8_span(s, pos, max_chars);
                out.push_back(s.substr(pos, n));
                pos += n;
            }
            continue;
        }
        if (!cur.empty() && cur.size() + 1 + s.size() > max_chars) flush();
        if (!cur.empty()) cur += ' ';
        cur += s;
    }
    flush();
    return out;
}

std::string tts_request_json(const std::string& model, const std::string& voice,
                             const std::string& input) {
    std::string out = "{\"model\":\"";
    json_escape_into(model, &out);
    out += "\",\"input\":\"";
    json_escape_into(input, &out);
    out += "\",\"response_format\":\"mp3\"";
    if (!voice.empty()) {
        out += ",\"voice\":\"";
        json_escape_into(voice, &out);
        out += "\"";
    }
    out += "}";
    return out;
}

bool event_finalizes(bool is_delta_done, bool display_text_ready_false) {
    return is_delta_done || !display_text_ready_false;
}

bool agent_status_combine(const char* activity_code, const char* status,
                          const char* label, AgentStatus* out) {
    if (!out) return false;
    *out = AgentStatus{};
    auto usable = [](const char* v) { return v && v[0]; };
    const bool from_code = usable(activity_code);
    const char* code = from_code ? activity_code
                                 : (usable(status) ? status : nullptr);
    if (!code) return false;
    out->present = true;
    std::snprintf(out->code, sizeof(out->code), "%s", code);
    if (usable(label)) {
        std::snprintf(out->label, sizeof(out->label), "%s", label);
    }
    // Busy semantics, ported from the reference firmware's on_event:
    // an activity code means busy unless it names the idle states; a
    // bare status means busy unless the task completed or failed.
    if (from_code) {
        out->busy = std::strcmp(code, "online") != 0 &&
                    std::strcmp(code, "idle") != 0;
    } else {
        out->busy = std::strcmp(code, "completed") != 0 &&
                    std::strcmp(code, "failed") != 0;
    }
    return true;
}

}  // namespace proto
}  // namespace aiwatchos_net
