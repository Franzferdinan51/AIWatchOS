// Host tests for aiwatchos_net's pure wire-format logic (net_proto.cpp).
// No ESP-IDF or hardware needed — plain g++:
//
//   g++ -std=c++17 -fno-exceptions -fno-rtti \
//       -I components/aiwatchos_net/include \
//       components/aiwatchos_net/src/net_proto.cpp \
//       components/aiwatchos_net/test/net_logic_test.cpp \
//       -o /tmp/net_logic_test && /tmp/net_logic_test
//
// Covers the byte-exact formats the Muse link shares with the reference
// gadget firmware: base64, the streaming WAV header, query escaping, host
// derivation, and the provisioning-string parser.
#include "aiwatchos_net/net_proto.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using namespace aiwatchos_net::proto;

static int g_failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

static std::string b64(const char* s) {
    char out[64];
    size_t n = base64_encode(reinterpret_cast<const uint8_t*>(s),
                             std::strlen(s), out);
    return std::string(out, n);
}

int main() {
    // --- base64 (RFC 4648 vectors; gadget muse_hatch_base64 semantics) ---
    CHECK(b64("") == "");
    CHECK(b64("f") == "Zg==");
    CHECK(b64("fo") == "Zm8=");
    CHECK(b64("foo") == "Zm9v");
    CHECK(b64("foob") == "Zm9vYg==");
    CHECK(b64("foobar") == "Zm9vYmFy");
    CHECK(b64_len(6144) == 8192);            // one note part -> one chunk
    CHECK(b64_len(0) == 0);
    CHECK(b64_len(1) == 4);
    {  // a full note part encodes with NO padding (6144 divisible by 3)
        static uint8_t part[kNotePartPcmBytes];
        static char out[kBodyChunkBytes + 1];
        for (size_t i = 0; i < sizeof(part); ++i) part[i] = uint8_t(i * 7);
        size_t n = base64_encode(part, sizeof(part), out);
        CHECK(n == kBodyChunkBytes);
        CHECK(std::strchr(out, '=') == nullptr);
    }

    // --- note framing constants match the reference protocol ---
    CHECK(std::string(kNoteHead).find("\"output_modality\":\"text\"") !=
          std::string::npos);
    CHECK(std::string(kNoteHead).find("\"mime_type\":\"audio/wav\"") !=
          std::string::npos);
    CHECK(std::string(kNoteTail) == "\"}]}");
    CHECK(kNotePartPcmBytes == 6144);

    // --- WAV header: streaming sizes, PCM16 mono @ rate ---
    {
        uint8_t h[kWavHeaderBytes];
        wav_header(h, 16000);
        CHECK(!std::memcmp(h, "RIFF", 4));
        CHECK(h[4] == 0xFF && h[5] == 0xFF && h[6] == 0xFF && h[7] == 0xFF);
        CHECK(!std::memcmp(h + 8, "WAVEfmt ", 8));
        CHECK(h[16] == 16 && h[17] == 0);                    // fmt chunk size
        CHECK(h[20] == 1 && h[21] == 0);                     // PCM
        CHECK(h[22] == 1 && h[23] == 0);                     // mono
        CHECK(h[24] == 0x80 && h[25] == 0x3E);               // 16000 le
        CHECK(h[28] == 0x00 && h[29] == 0x7D);               // 32000 le
        CHECK(h[32] == 2 && h[33] == 0);                     // block align
        CHECK(h[34] == 16 && h[35] == 0);                    // bits
        CHECK(!std::memcmp(h + 36, "data", 4));
        CHECK(h[40] == 0xFF && h[43] == 0xFF);
    }

    // --- query escaping (RFC 3986 unreserved + !~*'()) ---
    CHECK(query_escape("abc-_.~!*'()") == "abc-_.~!*'()");
    CHECK(query_escape("vm id") == "vm%20id");
    CHECK(query_escape("a/b+c=d&e") == "a%2Fb%2Bc%3Dd%26e");
    CHECK(query_escape("") == "");

    // --- host derivation ---
    CHECK(host_from_ws_url("wss://abc123.hatch.example/") == "abc123.hatch.example");
    CHECK(host_from_ws_url("wss://h.example:443/x") == "h.example");
    CHECK(host_from_ws_url("not a url") == "");
    CHECK(host_from_ws_url("") == "");
    CHECK(derive_host("explicit.h", "wss://u.h/", "vm1", "d.example") == "explicit.h");
    CHECK(derive_host("", "wss://u.h/", "vm1", "d.example") == "u.h");
    CHECK(derive_host("", "", "vm1", "d.example") == "vm1.d.example");
    CHECK(derive_host("", "", "", "d.example") == "");
    CHECK(derive_host("", "", "vm1", "") == "");

    // --- setup-string parsing ---
    {
        SetupFields f;
        CHECK(parse_setup_string(
            "aiwatchos://setup?ssid=Home%20Net&pass=p%40ss+word&token=tok123"
            "&vm=vm42&host=h.example&domain=d.example&vmtoken=vt9", &f));
        CHECK(f.has_ssid && f.ssid == "Home Net");
        CHECK(f.has_pass && f.pass == "p@ss word");
        CHECK(f.has_token && f.token == "tok123");
        CHECK(f.has_vm && f.vm == "vm42");
        CHECK(f.has_host && f.host == "h.example");
        CHECK(f.has_domain && f.domain == "d.example");
        CHECK(f.has_vm_token && f.vm_token == "vt9");
    }
    {
        SetupFields f;
        CHECK(parse_setup_string("aiwatchos://setup?ssid=Only", &f));
        CHECK(f.has_ssid && !f.has_pass && !f.has_token);
        CHECK(!parse_setup_string("https://setup?ssid=X", &f));
        CHECK(!parse_setup_string("aiwatchos://other?ssid=X", &f));
        CHECK(!parse_setup_string("", &f));
        CHECK(!parse_setup_string(nullptr, &f));
    }

    // --- Agent/task status combine (M2) -----------------------------------
    // Fixtures are subscribe-stream NDJSON lines; the field triples below
    // are what the firmware's cJSON extraction hands agent_status_combine
    // (activity_code, status, label — extraction yields "" for absent or
    // non-string fields, which the combine treats as absent):
    //   {"type":"event","seq":7,"event":"agent.status","payload":{"status":"idle"}}
    //   {"type":"event","seq":8,"event":"agent.status","payload":{"activity_code":"searching"}}
    //   {"type":"event","seq":9,"event":"task.status","payload":{"status":"running","label":"Summarize inbox"}}
    //   {"type":"event","seq":10,"event":"task.status","payload":{"status":"completed"}}
    //   {"type":"event","seq":11,"event":"agent.status","payload":{"activity_code":42}}
    {
        AgentStatus st;
        // Bare status, no label. Busy follows the reference firmware's
        // status-only rule (busy unless completed/failed) — even for
        // "idle": on the wire a status-only agent.status is turn
        // activity; the UI's pose map (muse_captions) is what renders
        // "idle" as rest. The code's idle spellings only relax busy
        // when they arrive AS an activity_code (next checks).
        CHECK(agent_status_combine(nullptr, "idle", nullptr, &st));
        CHECK(st.present && st.busy && std::string(st.code) == "idle");
        CHECK(st.label[0] == '\0');
        // Activity code wins over status and drives busy.
        CHECK(agent_status_combine("searching", "idle", nullptr, &st));
        CHECK(st.busy && std::string(st.code) == "searching");
        // The code's idle spellings are not busy.
        CHECK(agent_status_combine("online", nullptr, nullptr, &st));
        CHECK(!st.busy);
        CHECK(agent_status_combine("idle", nullptr, nullptr, &st));
        CHECK(!st.busy);
        // Empty activity_code falls through to status (companion _text).
        CHECK(agent_status_combine("", "working", nullptr, &st));
        CHECK(st.busy && std::string(st.code) == "working");
        // Task status: label retained; running is busy.
        CHECK(agent_status_combine(nullptr, "running", "Summarize inbox", &st));
        CHECK(st.busy && std::string(st.label) == "Summarize inbox");
        // Terminal task states are not busy.
        CHECK(agent_status_combine(nullptr, "completed", nullptr, &st));
        CHECK(!st.busy);
        CHECK(agent_status_combine(nullptr, "failed", nullptr, &st));
        CHECK(!st.busy);
        // Malformed/absent: non-string activity_code extracts as "", an
        // empty payload has neither field — combine refuses both.
        CHECK(!agent_status_combine("", "", nullptr, &st));
        CHECK(!st.present);
        CHECK(!agent_status_combine(nullptr, nullptr, nullptr, &st));
        CHECK(!agent_status_combine(nullptr, nullptr, "orphan label", &st));
        // An over-long code is capped, never overflowing the field.
        std::string big(200, 'x');
        CHECK(agent_status_combine(big.c_str(), nullptr, nullptr, &st));
        CHECK(std::strlen(st.code) == sizeof(st.code) - 1);
        CHECK(!agent_status_combine("thinking", nullptr, nullptr, nullptr));
    }

    if (g_failures == 0) std::printf("net_logic_test: all checks passed\n");
    // --- heard transcript cleaning (M5, gadget firmware on_row rules) ---
    {
        char out[64];
        CHECK(heard_transcript_clean("turn on the lights", out, sizeof(out)));
        CHECK(std::strcmp(out, "turn on the lights") == 0);
        // The server's attachment suffix is cut.
        CHECK(heard_transcript_clean("hello world\n[file:audio/wav 42]",
                                     out, sizeof(out)));
        CHECK(std::strcmp(out, "hello world") == 0);
        // The pre-transcription placeholder is not a transcript.
        CHECK(!heard_transcript_clean("[Voice note]", out, sizeof(out)));
        CHECK(out[0] == '\0');
        CHECK(!heard_transcript_clean("[Voice note]\n[file:audio/wav 1]",
                                      out, sizeof(out)));
        // Empty / whitespace / null are unusable.
        CHECK(!heard_transcript_clean("", out, sizeof(out)));
        CHECK(!heard_transcript_clean("  \n ", out, sizeof(out)));
        CHECK(!heard_transcript_clean(nullptr, out, sizeof(out)));
        // Surrounding whitespace is trimmed.
        CHECK(heard_transcript_clean("  hi there \n", out, sizeof(out)));
        CHECK(std::strcmp(out, "hi there") == 0);
        // Cap truncation lands on a UTF-8 boundary (\xC3\xA9 = e-acute).
        char small[5];
        CHECK(heard_transcript_clean("ab\xC3\xA9xyz", small, sizeof(small)));
        CHECK(std::strcmp(small, "ab\xC3\xA9") == 0);
        char tiny[4];
        CHECK(heard_transcript_clean("ab\xC3\xA9xyz", tiny, sizeof(tiny)));
        CHECK(std::strcmp(tiny, "ab") == 0);
    }

    return g_failures ? 1 : 0;
}
