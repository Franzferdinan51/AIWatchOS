// Spoken replies (TTS) — device glue.
//
// When a speech provider is configured (aiwnet TTS key set), each
// completed Muse reply is cleaned (proto::speech_clean), chunked
// (proto::speech_chunks) and POSTed chunk by chunk to the provider's
// OpenAI-shaped speech endpoint (POST {base}/v1/audio/speech, Bearer
// key, JSON {model, voice, input, response_format:"mp3"}). The MP3
// streams into a PSRAM buffer and plays through the speech sink the
// Muse app registered (muse_play_reply). This runs on the link task:
// the UI loop never blocks on a fetch, playback paces itself through
// the sink, and the face's SPEAKING state is driven from the real
// playback window (muse_link_speaking()).
//
// Fail-closed throughout: unconfigured, no sink, HTTP error, an MP3
// over the 512 KB cap, PSRAM exhaustion, or a playback refusal all
// drop the reply (or its remaining chunks) to text silently. Speech
// must never break the text path.
//
// The API key is read from config at fetch time and never logged.
#include "muse_tts.hpp"

#include <atomic>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "aiwatchos_net/net.hpp"
#include "aiwatchos_net/net_proto.hpp"

namespace aiwatchos_net {
namespace {

const char* kTag = "aiw-tts";
// One chunk is ~1000 chars (~a minute of speech); the fetch itself
// should take seconds. 30 s bounds a wedged provider so the link
// task (and the next turn) is never held hostage.
constexpr int kFetchTimeoutMs = 30000;

SpeechSinkFn g_sink = nullptr;
void* g_sink_ctx = nullptr;
// Pipeline sequence (link-task local); the speaking flag mirrors the
// machine's kPlaying window for the UI task's lock-free reads.
proto::SpeechMachine g_machine;
std::atomic<bool> g_speaking{false};

// Streaming MP3 accumulator: PSRAM-only, doubling growth, hard cap.
struct Mp3Sink {
    uint8_t* buf = nullptr;
    size_t len = 0;
    size_t cap = 0;
    bool failed = false;   // oversize or PSRAM OOM: abort the fetch
};

esp_err_t on_http_event(esp_http_client_event_t* evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data ||
        evt->data_len <= 0) {
        return ESP_OK;
    }
    auto* sink = static_cast<Mp3Sink*>(evt->user_data);
    if (!sink || sink->failed) return ESP_FAIL;
    const size_t add = static_cast<size_t>(evt->data_len);
    // The cap check comes first and is exact: a response that would
    // grow past 512 KB fails the fetch, it is never truncated into a
    // half-playable clip.
    if (sink->len + add > proto::kSpeechMp3CapBytes) {
        sink->failed = true;
        return ESP_FAIL;
    }
    if (sink->len + add > sink->cap) {
        size_t want = sink->cap ? sink->cap : 64 * 1024;
        while (want < sink->len + add) want *= 2;
        if (want > proto::kSpeechMp3CapBytes) {
            want = proto::kSpeechMp3CapBytes;
        }
        auto* grown = static_cast<uint8_t*>(heap_caps_realloc(
            sink->buf, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!grown) {
            sink->failed = true;   // PSRAM exhausted: fail closed
            return ESP_FAIL;
        }
        sink->buf = grown;
        sink->cap = want;
    }
    std::memcpy(sink->buf + sink->len, evt->data, add);
    sink->len += add;
    return ESP_OK;
}

// One text chunk -> one MP3. On success the caller owns a PSRAM buffer
// (release with heap_caps_free). Any failure returns false.
bool fetch_chunk(const NetConfig& cfg, const std::string& text,
                 uint8_t** out_mp3, size_t* out_len) {
    const std::string url = proto::speech_url(cfg.tts_base_url);
    if (url.empty()) return false;
    const std::string body =
        proto::speech_body(cfg.tts_model, cfg.tts_voice, text);

    Mp3Sink sink;
    esp_http_client_config_t hcfg{};
    hcfg.url = url.c_str();
    hcfg.method = HTTP_METHOD_POST;
    hcfg.event_handler = &on_http_event;
    hcfg.user_data = &sink;
    hcfg.timeout_ms = kFetchTimeoutMs;
    hcfg.disable_auto_redirect = true;
    hcfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    if (!client) return false;
    const std::string auth = "Bearer " + cfg.tts_api_key;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body.c_str(),
                                   static_cast<int>(body.size()));
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || sink.failed || sink.len == 0) {
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "speech fetch failed: %s", esp_err_to_name(err));
        } else if (status != 200) {
            ESP_LOGW(kTag, "speech fetch HTTP %d", status);
        }
        heap_caps_free(sink.buf);
        return false;
    }
    *out_mp3 = sink.buf;
    *out_len = sink.len;
    return true;
}

}  // namespace

void muse_link_set_speech_sink(SpeechSinkFn fn, void* ctx) {
    g_sink = fn;
    g_sink_ctx = ctx;
}

bool muse_link_speaking() { return g_speaking.load(); }

bool tts_speak_reply(const std::string& reply_text) {
    const NetConfig& cfg = net_config();
    // The gate: no key (or no playback sink) -> no request is ever
    // attempted and the reply stays text, exactly as before the port.
    if (!cfg.tts_configured() || !g_sink) return false;

    const std::string cleaned = proto::speech_clean(reply_text);
    if (cleaned.empty()) return false;
    const auto chunks = proto::speech_chunks(
        cleaned, proto::kSpeechChunkChars, proto::kSpeechMaxChunks);
    if (chunks.empty()) return false;

    g_machine.reset();
    g_machine.begin(chunks.size());
    for (const auto& chunk : chunks) {
        uint8_t* mp3 = nullptr;
        size_t len = 0;
        if (!fetch_chunk(cfg, chunk, &mp3, &len)) {
            g_machine.fetch_failed();
            ESP_LOGW(kTag, "spoken reply dropped to text (fetch)");
            return false;
        }
        g_machine.fetch_ok();          // -> kPlaying
        g_speaking.store(true);
        const bool played = g_sink(mp3, len, g_sink_ctx);
        g_speaking.store(false);
        heap_caps_free(mp3);
        if (!played) {
            g_machine.play_failed();
            ESP_LOGW(kTag, "spoken reply dropped to text (playback)");
            return false;
        }
        g_machine.play_done();         // -> kFetching (more) or kIdle
    }
    ESP_LOGI(kTag, "spoke reply in %u chunk(s)",
             static_cast<unsigned>(chunks.size()));
    return true;
}

}  // namespace aiwatchos_net
