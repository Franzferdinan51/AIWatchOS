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
            else if (key == "ttsurl") { out->tts_base_url = val; out->has_tts_base_url = true; }
            else if (key == "ttskey") { out->tts_api_key = val; out->has_tts_api_key = true; }
            else if (key == "ttsmodel") { out->tts_model = val; out->has_tts_model = true; }
            else if (key == "ttsvoice") { out->tts_voice = val; out->has_tts_voice = true; }
        }
        if (amp == query.size()) break;
        pos = amp + 1;
    }
    return true;
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

// --- Spoken replies ---------------------------------------------------------

namespace {

// UTF-8 helpers (code-point counting, mirroring muse_captions' rules:
// lengths are code points; invalid bytes count as one each).
size_t utf8_cp_len(const std::string& s) {
    size_t n = 0;
    for (char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

// Byte length of the first `cp` code points of s (all of s if shorter).
size_t utf8_prefix_bytes(const std::string& s, size_t cp) {
    size_t seen = 0;
    size_t i = 0;
    while (i < s.size() && seen < cp) {
        ++seen;
        ++i;
        while (i < s.size() &&
               (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
            ++i;
        }
    }
    return i;
}

// Byte length of the code point starting at s[i] (1 for ASCII; a lead
// byte without its continuations also counts as one bad byte).
size_t utf8_cp_bytes(const std::string& s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t n = 1;
    if (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    if (n > 1 && (i + n > s.size() ||
                 (static_cast<unsigned char>(s[i + 1]) & 0xC0) != 0x80)) {
        n = 1;
    }
    return n;
}

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

}  // namespace

std::string speech_clean(const std::string& text) {
    // Mirror of muse_captions.hpp captions_detail::clean() (which see
    // for the Dart provenance); keep the two in step.
    std::string value = text;
    // 1. ```...``` (non-greedy) -> ' '. An unclosed fence does not
    // match; its backticks fall to step 3.
    for (size_t open = value.find("```"); open != std::string::npos;) {
        size_t close = value.find("```", open + 3);
        if (close == std::string::npos) break;
        value.replace(open, close + 3 - open, " ");
        open = value.find("```", open + 1);
    }
    // 2. [text](url) -> text.
    for (size_t i = 0; i < value.size();) {
        if (value[i] != '[') { ++i; continue; }
        size_t rb = value.find(']', i + 1);
        if (rb == std::string::npos || rb == i + 1) { ++i; continue; }
        if (rb + 1 >= value.size() || value[rb + 1] != '(') { ++i; continue; }
        size_t rp = value.find(')', rb + 2);
        if (rp == std::string::npos || rp == rb + 2) { ++i; continue; }
        std::string link_text = value.substr(i + 1, rb - i - 1);
        value.replace(i, rp + 1 - i, link_text);
        i += link_text.size();
    }
    // 3. Remove every [*_~`#>].
    std::string stripped;
    stripped.reserve(value.size());
    for (char c : value) {
        if (c == '*' || c == '_' || c == '~' || c == '`' || c == '#' ||
            c == '>') {
            continue;
        }
        stripped.push_back(c);
    }
    // 4. Collapse whitespace runs to one space; trim both ends.
    std::string out;
    out.reserve(stripped.size());
    bool pending_space = false;
    for (char c : stripped) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
            c == '\v') {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(c);
    }
    return out;
}

std::vector<std::string> speech_chunks(const std::string& cleaned,
                                       size_t max_chars,
                                       size_t max_chunks) {
    std::vector<std::string> out;
    if (max_chars == 0 || max_chunks == 0) return out;
    std::string rest = cleaned;
    while (!rest.empty() && out.size() < max_chunks) {
        // Leading spaces never start a chunk (cleaned text has none,
        // but a remainder after a cut can).
        size_t lead = 0;
        while (lead < rest.size() && rest[lead] == ' ') ++lead;
        if (lead > 0) rest.erase(0, lead);
        if (rest.empty()) break;
        if (utf8_cp_len(rest) <= max_chars) {
            out.push_back(rest);
            break;
        }
        const size_t win = utf8_prefix_bytes(rest, max_chars);
        // Scan the window code point by code point for the LAST usable
        // sentence ender and the LAST space. An ender (. ! ? U+2026)
        // counts only when followed by a space or by the window's end,
        // so "3.14" never splits mid-number.
        size_t ender_cut = 0;   // byte offset just past the ender
        size_t space_cut = 0;   // byte offset OF the space
        for (size_t i = 0; i < win;) {
            const size_t n = utf8_cp_bytes(rest, i);
            const unsigned char c = static_cast<unsigned char>(rest[i]);
            const bool is_ender =
                (n == 1 && (c == '.' || c == '!' || c == '?')) ||
                (n == 3 && c == 0xE2 && i + 2 < rest.size() &&
                 static_cast<unsigned char>(rest[i + 1]) == 0x80 &&
                 static_cast<unsigned char>(rest[i + 2]) == 0xA6);
            if (is_ender) {
                const size_t after = i + n;
                if (after >= win || rest[after] == ' ') ender_cut = after;
            } else if (n == 1 && c == ' ') {
                space_cut = i;
            }
            i += n;
        }
        size_t cut = win;
        if (ender_cut > 0) cut = ender_cut;
        else if (space_cut > 0) cut = space_cut;
        std::string chunk = rest.substr(0, cut);
        while (!chunk.empty() && chunk.back() == ' ') chunk.pop_back();
        if (!chunk.empty()) out.push_back(chunk);
        rest.erase(0, cut);
    }
    return out;
}

std::string speech_url(const std::string& base_url) {
    std::string base = base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.empty()) return "";
    return base + "/v1/audio/speech";
}

std::string speech_body(const std::string& model, const std::string& voice,
                        const std::string& input) {
    std::string body = "{\"model\":\"";
    json_escape_into(model, &body);
    body += "\",\"voice\":\"";
    json_escape_into(voice, &body);
    body += "\",\"input\":\"";
    json_escape_into(input, &body);
    body += "\",\"response_format\":\"mp3\"}";
    return body;
}

void SpeechMachine::begin(size_t chunks) {
    chunks_total = chunks;
    chunks_done = 0;
    state = chunks > 0 ? SpeechState::kFetching : SpeechState::kIdle;
}

void SpeechMachine::fetch_ok() {
    if (state == SpeechState::kFetching) state = SpeechState::kPlaying;
}

void SpeechMachine::fetch_failed() {
    if (state == SpeechState::kFetching) state = SpeechState::kFailed;
}

void SpeechMachine::play_done() {
    if (state != SpeechState::kPlaying) return;
    ++chunks_done;
    state = chunks_done < chunks_total ? SpeechState::kFetching
                                       : SpeechState::kIdle;
}

void SpeechMachine::play_failed() {
    if (state == SpeechState::kPlaying) state = SpeechState::kFailed;
}

void SpeechMachine::reset() {
    state = SpeechState::kIdle;
    chunks_total = 0;
    chunks_done = 0;
}

}  // namespace proto
}  // namespace aiwatchos_net
