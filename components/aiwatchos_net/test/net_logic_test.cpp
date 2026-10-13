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

    // --- Spoken replies (TTS): clean / chunk / url / body / machine ---
    {
        // speech_clean mirrors the caption cleaner (same vectors as
        // the root suite's M3 checks) but never clips.
        CHECK(speech_clean("Here is `code` and **bold** text") ==
              "Here is code and bold text");
        CHECK(speech_clean("Before\n\n```dart\nvoid main() {}\n```\n\nAfter") ==
              "Before After");
        CHECK(speech_clean("See [the docs](https://example.com/x) now") ==
              "See the docs now");
        CHECK(speech_clean("# Title\n> quoted _line_ ~gone~") ==
              "Title quoted line gone");
        CHECK(speech_clean("  spaced   out\t\n lines  ") == "spaced out lines");
        CHECK(speech_clean("a ``` b") == "a b");
        CHECK(speech_clean("").empty());
        CHECK(speech_clean("```\nonly code\n```").empty());
        const std::string long_clean(300, 'a');
        CHECK(speech_clean(long_clean) == long_clean);   // no 240 clip

        // Chunks: short and exact-cap inputs pass through whole.
        CHECK(speech_chunks("", 1000, 3).empty());
        CHECK(speech_chunks("hello", 1000, 3).size() == 1);
        CHECK(speech_chunks(std::string(1000, 'a'), 1000, 3).size() == 1);
        CHECK(speech_chunks("hello", 0, 3).empty());
        CHECK(speech_chunks("hello", 1000, 0).empty());
        {
            auto c = speech_chunks(std::string(1001, 'a'), 1000, 3);
            CHECK(c.size() == 2 && c[0].size() == 1000 && c[1] == "a");
        }
        // Sentence boundaries: the cut lands exactly on the ender, and
        // the next chunk starts cleanly at the next sentence.
        {
            std::string s = std::string(600, 'a') + ". " +
                            std::string(600, 'b') + ". " +
                            std::string(600, 'c') + ".";
            auto c = speech_chunks(s, 1000, 3);
            CHECK(c.size() == 3);
            CHECK(c[0].size() == 601 && c[0].back() == '.');
            CHECK(c[1].size() == 601 && c[1].front() == 'b');
            CHECK(c[2] == std::string(600, 'c') + ".");
        }
        // A decimal point is not a sentence boundary.
        {
            std::string s = "Pi is 3.14159 and that is fine. " +
                            std::string(2000, 'x');
            auto c = speech_chunks(s, 100, 5);
            CHECK(!c.empty());
            CHECK(c[0] == "Pi is 3.14159 and that is fine.");
        }
        // Word-boundary fallback when no ender is in the window.
        {
            std::string s = std::string(999, 'a') + " " + std::string(500, 'b');
            auto c = speech_chunks(s, 1000, 3);
            CHECK(c.size() == 2 && c[0].size() == 999 && c[1].size() == 500);
        }
        // No boundary at all: hard cuts at the cap; the chunk budget
        // drops the remainder.
        {
            auto c = speech_chunks(std::string(3500, 'x'), 1000, 3);
            CHECK(c.size() == 3 && c[0].size() == 1000 && c[2].size() == 1000);
            auto c2 = speech_chunks(std::string(2500, 'x'), 1000, 3);
            CHECK(c2.size() == 3 && c2[2].size() == 500);
        }
        // UTF-8: caps count code points and cuts never split one.
        {
            std::string s;
            for (int i = 0; i < 1500; ++i) s += "\xC3\xA9";   // U+00E9
            auto c = speech_chunks(s, 1000, 3);
            CHECK(c.size() == 2 && c[0].size() == 2000 && c[1].size() == 1000);
        }
        {
            // U+2026 is an ender, but followed directly by 'b' it is not
            // a boundary: the splitter falls back to the hard cut at
            // exactly 1000 code points, ellipsis intact.
            std::string s = std::string(600, 'a') + "\xE2\x80\xA6" +
                            std::string(600, 'b');
            auto c = speech_chunks(s, 1000, 3);
            CHECK(c.size() == 2);
            size_t cps = 0;
            for (char ch : c[0]) {
                if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++cps;
            }
            CHECK(cps == 1000);
            CHECK(c[0].find("\xE2\x80\xA6") != std::string::npos);
        }

        // URL join: the base is the full prefix; trailing slashes go.
        CHECK(speech_url("https://openrouter.ai/api") ==
              "https://openrouter.ai/api/v1/audio/speech");
        CHECK(speech_url("https://openrouter.ai/api/") ==
              "https://openrouter.ai/api/v1/audio/speech");
        CHECK(speech_url("http://127.0.0.1:8080//") ==
              "http://127.0.0.1:8080/v1/audio/speech");
        CHECK(speech_url("").empty());
        CHECK(speech_url("///").empty());

        // Request body: exact shape + JSON escaping. (Key-empty -> no
        // request attempted is enforced one layer up: tts_speak_reply
        // returns before any fetch when the config has no key; the
        // config seam below is the host-testable half of that gate.)
        CHECK(speech_body("m", "v", "hi") ==
              "{\"model\":\"m\",\"voice\":\"v\",\"input\":\"hi\","
              "\"response_format\":\"mp3\"}");
        CHECK(speech_body("m", "v", "a\"b\\c\nd") ==
              "{\"model\":\"m\",\"voice\":\"v\",\"input\":\"a\\\"b\\\\c\\nd\","
              "\"response_format\":\"mp3\"}");
        CHECK(speech_body("m", "v", std::string("x") + char(0x01)) ==
              "{\"model\":\"m\",\"voice\":\"v\",\"input\":\"x\\u0001\","
              "\"response_format\":\"mp3\"}");

        // Config seam: the setup string carries the TTS fields.
        {
            SetupFields f;
            CHECK(parse_setup_string(
                "aiwatchos://setup?ssid=Cafe&ttsurl=https%3A%2F%2Fapi.example.com"
                "&ttskey=k-123&ttsmodel=m1&ttsvoice=nova", &f));
            CHECK(f.has_ssid && f.ssid == "Cafe");
            CHECK(f.has_tts_base_url &&
                  f.tts_base_url == "https://api.example.com");
            CHECK(f.has_tts_api_key && f.tts_api_key == "k-123");
            CHECK(f.has_tts_model && f.tts_model == "m1");
            CHECK(f.has_tts_voice && f.tts_voice == "nova");
            SetupFields g;
            CHECK(parse_setup_string("aiwatchos://setup?ssid=Cafe", &g));
            CHECK(!g.has_tts_api_key && !g.has_tts_base_url);
        }

        // Speech machine: the transitions muse_tts.cpp drives.
        {
            SpeechMachine m;
            CHECK(m.state == SpeechState::kIdle && !m.speaking() && !m.active());
            m.fetch_ok();                       // stray events are no-ops
            CHECK(m.state == SpeechState::kIdle);
            m.play_done();
            CHECK(m.state == SpeechState::kIdle && m.chunks_done == 0);
            m.begin(0);                         // nothing to say: stays idle
            CHECK(m.state == SpeechState::kIdle);
            m.begin(2);
            CHECK(m.state == SpeechState::kFetching && m.active() &&
                  !m.speaking());
            m.fetch_ok();
            CHECK(m.state == SpeechState::kPlaying && m.speaking());
            m.play_done();                      // one chunk left: fetch again
            CHECK(m.state == SpeechState::kFetching && m.chunks_done == 1);
            m.fetch_ok();
            m.play_done();
            CHECK(m.state == SpeechState::kIdle && m.chunks_done == 2);
            // Failure lands in kFailed from either active state...
            m.begin(1);
            m.fetch_failed();
            CHECK(m.state == SpeechState::kFailed && !m.speaking() &&
                  !m.active());
            m.begin(1);                         // ...and a new reply recovers
            CHECK(m.state == SpeechState::kFetching);
            m.fetch_ok();
            m.play_failed();
            CHECK(m.state == SpeechState::kFailed);
            m.reset();
            CHECK(m.state == SpeechState::kIdle && m.chunks_total == 0);
        }
    }

    if (g_failures == 0) std::printf("net_logic_test: all checks passed\n");
    return g_failures ? 1 : 0;
}
