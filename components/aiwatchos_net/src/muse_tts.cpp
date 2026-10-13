// Cloud-TTS spoken replies (M10) — the watch-side client for the
// companion's opt-in OpenRouter path (openrouter_tts.dart), generalized
// to any OpenAI-shaped speech endpoint from provisioning (net.hpp).
//
// Shape of the engine:
//   - The Muse app detects a settled reply, cleans it with the
//     companion caption cleaner (full text, no display clip) and hands
//     it to muse_tts_speak_text(). Latest wins: unspeaked text is
//     replaced, never queued behind stale speech.
//   - A dedicated task chunks the text (proto::tts_chunks, 1800 chars),
//     POSTs each chunk (esp_http_client + esp_crt_bundle, the
//     muse_account.cpp pattern), streams the MP3 into a PSRAM buffer
//     with a hard 1 MB cap (fail-closed: an oversize or failed chunk is
//     dropped, never spilled to DRAM), and plays it through the player
//     the Muse app registered (muse_play_reply, via the seam below)
//     before fetching the next chunk — fetch-then-play per chunk,
//     exactly like the companion.
//   - One-TLS rule: a chunk fetch starts only while the Muse link is
//     kLinked (Noise handshake long done) AND no turn upload is in
//     flight; the wait is bounded (20 s) and stop-aware. The idle
//     subscribe session coexists with the fetch, as it already does
//     with the weather service's HTTPS calls.
//   - Barge-in: muse_tts_stop() bumps the generation; the task drops
//     pending text and remaining chunks, and the app-side player
//     aborts the in-flight MP3 itself (muse_speech_stop).
//   - Fail-soft everywhere: toggle off, unprovisioned (empty key), no
//     player, HTTP error — speech simply doesn't happen; the text
//     reply on screen is never affected. The API key travels only in
//     the Authorization header and is never logged.
#include "aiwatchos_net/net.hpp"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "aiwatchos_net/net_proto.hpp"

namespace aiwatchos_net {

// Defined in muse_link.cpp (declared here, the net_manager pattern).
bool muse_link_upload_active();

namespace {

const char* kTag = "aiw-tts";

// Per-chunk MP3 cap: speech MP3 runs ~16 KB/s at 128 kbps, so 1 MB is
// over a minute of audio for one <=1800-char chunk. PSRAM-only.
constexpr size_t kMaxMp3Bytes = 1024 * 1024;
// Bounded wait for a quiet link before each chunk fetch (one-TLS rule).
constexpr int kGateWaitMs = 20000;
// The companion's per-request timeout.
constexpr int kHttpTimeoutMs = 60000;

SemaphoreHandle_t g_mutex = nullptr;
SemaphoreHandle_t g_wake = nullptr;   // binary: new text / stop posted
std::string g_pending;                // cleaned text awaiting speech
uint32_t g_generation = 0;            // bumped on stop / replace
std::atomic<bool> g_speaking{false};
SpeechPlayerFn g_player = nullptr;
void* g_player_ctx = nullptr;
bool g_started = false;

uint32_t current_generation() {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const uint32_t gen = g_generation;
    xSemaphoreGive(g_mutex);
    return gen;
}

struct Mp3Sink {
    uint8_t* buf = nullptr;
    size_t len = 0;
    bool overflow = false;
};

esp_err_t on_tts_http_event(esp_http_client_event_t* evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data ||
        evt->data_len <= 0) {
        return ESP_OK;
    }
    auto* sink = static_cast<Mp3Sink*>(evt->user_data);
    if (!sink || !sink->buf) return ESP_OK;
    if (sink->len + static_cast<size_t>(evt->data_len) > kMaxMp3Bytes) {
        sink->overflow = true;
        return ESP_FAIL;   // abort the transfer; the chunk is dropped
    }
    std::memcpy(sink->buf + sink->len, evt->data,
                static_cast<size_t>(evt->data_len));
    sink->len += static_cast<size_t>(evt->data_len);
    return ESP_OK;
}

// One chunk -> MP3 bytes in a fresh PSRAM buffer (caller frees with
// heap_caps_free). nullptr on any failure; only the HTTP status is
// logged (error bodies are never echoed, the key never leaves the
// Authorization header).
uint8_t* fetch_chunk(const NetConfig& cfg, const std::string& chunk,
                     size_t* out_len) {
    uint8_t* buf = static_cast<uint8_t*>(
        heap_caps_malloc(kMaxMp3Bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        ESP_LOGE(kTag, "mp3 buffer alloc failed");
        return nullptr;
    }
    Mp3Sink sink;
    sink.buf = buf;
    const std::string body =
        proto::tts_request_json(cfg.tts_model, cfg.tts_voice, chunk);
    esp_http_client_config_t hc{};
    hc.url = cfg.tts_url.c_str();
    hc.method = HTTP_METHOD_POST;
    hc.event_handler = &on_tts_http_event;
    hc.user_data = &sink;
    hc.timeout_ms = kHttpTimeoutMs;
    hc.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&hc);
    if (!client) {
        heap_caps_free(buf);
        return nullptr;
    }
    const std::string auth = "Bearer " + cfg.tts_key;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body.c_str(),
                                   static_cast<int>(body.size()));
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || sink.overflow || sink.len == 0) {
        ESP_LOGW(kTag, "speech fetch failed (http %d, err %s)", status,
                 esp_err_to_name(err));
        heap_caps_free(buf);
        return nullptr;
    }
    *out_len = sink.len;
    return buf;
}

// One-TLS gate (see header): fetch only while linked with no turn
// upload in flight. Bounded and stop-aware.
bool wait_for_link(uint32_t gen) {
    const TickType_t start = xTaskGetTickCount();
    for (;;) {
        if (current_generation() != gen) return false;
        const LinkState s = muse_link_state();
        if (s == LinkState::kOff) return false;   // unconfigured: no speech
        if (s == LinkState::kLinked && !muse_link_upload_active()) {
            return true;
        }
        const int waited = static_cast<int>(
            (xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
        if (waited >= kGateWaitMs) return false;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void speak_now(const std::string& text, uint32_t gen) {
    const NetConfig cfg = net_config();   // snapshot; gen guards the text
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    SpeechPlayerFn player = g_player;
    void* pctx = g_player_ctx;
    xSemaphoreGive(g_mutex);
    if (!cfg.tts_speak || !cfg.tts_provisioned() || !player) return;
    const std::vector<std::string> chunks = proto::tts_chunks(text);
    if (chunks.empty()) return;
    g_speaking.store(true);
    for (const std::string& chunk : chunks) {
        if (current_generation() != gen) break;   // stopped / superseded
        if (!wait_for_link(gen)) break;
        size_t len = 0;
        uint8_t* mp3 = fetch_chunk(cfg, chunk, &len);
        if (!mp3) break;   // fail soft: remaining chunks are dropped
        if (current_generation() == gen) player(mp3, len, pctx);
        heap_caps_free(mp3);
    }
    g_speaking.store(false);
}

void tts_task(void*) {
    for (;;) {
        xSemaphoreTake(g_wake, portMAX_DELAY);
        std::string text;
        uint32_t gen = 0;
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        text = g_pending;
        g_pending.clear();
        gen = g_generation;
        xSemaphoreGive(g_mutex);
        if (!text.empty()) speak_now(text, gen);
    }
}

}  // namespace

// --- Public API (net.hpp) ----------------------------------------------------

bool muse_tts_speak_enabled() { return net_config().tts_speak; }

void muse_tts_set_speak_enabled(bool on) {
    NetConfig cfg = net_config();
    if (cfg.tts_speak == on) return;
    cfg.tts_speak = on;
    net_save_config(cfg);
}

bool muse_tts_provisioned() { return net_config().tts_provisioned(); }

bool muse_tts_speaking() { return g_speaking.load(); }

void muse_tts_stop() {
    if (!g_mutex) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    ++g_generation;
    g_pending.clear();
    xSemaphoreGive(g_mutex);
    if (g_wake) xSemaphoreGive(g_wake);
}

void muse_tts_speak_text(const char* cleaned_text) {
    if (!cleaned_text || !cleaned_text[0] || !g_mutex) return;
    const NetConfig& cfg = net_config();
    if (!cfg.tts_speak || !cfg.tts_provisioned() || !g_player) return;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    g_pending = cleaned_text;   // latest wins; never a queue of stale speech
    ++g_generation;             // supersedes any in-flight speech
    xSemaphoreGive(g_mutex);
    xSemaphoreGive(g_wake);
}

void muse_tts_set_player(SpeechPlayerFn fn, void* ctx) {
    g_player = fn;
    g_player_ctx = ctx;
}

// Starts the speech task. Called by net_start() alongside the link
// task (same configured-only path); idempotent.
void muse_tts_start() {
    if (g_started) return;
    g_started = true;
    g_mutex = xSemaphoreCreateMutex();
    g_wake = xSemaphoreCreateBinary();
    if (!g_mutex || !g_wake) {
        ESP_LOGE(kTag, "sync alloc failed");
        return;
    }
    xTaskCreate(tts_task, "aiw_tts", 16384, nullptr, 4, nullptr);
}

}  // namespace aiwatchos_net
