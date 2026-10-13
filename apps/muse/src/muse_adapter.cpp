// Muse app adapter — wraps the muse-gadget-206 voice interaction as an OS App.
// The original project's entry point is muse_app_run() in main/app.c, which runs
// a monolithic loop handling WiFi connection, Noise protocol handshake with the
// Muse/Hermes server, push-to-talk voice capture (ES7210 mic), and reply playback
// (ES8311 speaker). This adapter decomposes that into init/tick/render callbacks.
#include "aiwatchos_muse.hpp"
#include "muse_captions.hpp"  // companion caption/activity ports (M1/M2/M3)
#include "muse_pixel.h"       // muse-gadget-206 pixel avatar (M19)
#include "aiwatchos/colors.hpp"
#include "aiwatchos/font.hpp"
#include "minimp3.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef __ESPRESSIF_IDF__
#include "esp_heap_caps.h"
#include "aiwatchos_net/net.hpp"
#endif

namespace aiwatchos_muse {

constexpr uint32_t kTickRateMs = 40;   // match LVGL frame rate from board_waveshare_s3_206.c

// File-scope state for the callback-based app interface.
static MuseState g_state;
// Hal driven by this adapter (Board on device, injected fake in tests).
static aiwatchos::Hal* g_hal = nullptr;

// --- Muse Gloss face state (DESIGN_TOKENS sections 3+6, plan 3.3) ---
// The state scheme cross-fades with an exponential ease (tokens: rate
// 7/s) so colour changes bloom instead of snapping. Channels are kept
// as floats in 565 space (r/b 0..31, g 0..63): 5 stops x 3 channels.
struct SchemeF {
    float ch[5][3];
};
static SchemeF g_scheme_cur = {};
static bool g_scheme_init = false;
static uint64_t g_anim_ms = 0;          // face animation clock (tick-driven)

// muse_pixel avatar (M19): init is tried once; on allocation failure
// the interim primitives avatar (draw_jolly) stands in permanently.
static bool g_pixel_tried = false;
static bool g_pixel_ok = false;
// M10 speech state: the net component's TTS engine plays fetched MP3
// through speech_player() (below); g_speech_stop is the barge-in flag
// muse_play_reply polls per MP3 frame.
static std::atomic<bool> g_speech_playing{false};
static std::atomic<bool> g_speech_stop{false};
static aiwatchos::MuseState g_pose_state = aiwatchos::MuseState::kReady;
static uint64_t g_pose_since_ms = 0;
static uint64_t g_happy_until_ms = 0;   // pet reaction envelope (g_hal time)
static float g_seg[14] = {};            // level-meter segment envelopes

static void scheme_unpack(uint16_t c, float* out) {
    out[0] = static_cast<float>((c >> 11) & 0x1F);
    out[1] = static_cast<float>((c >> 5) & 0x3F);
    out[2] = static_cast<float>(c & 0x1F);
}
static uint16_t scheme_pack(const float* in) {
    int r = static_cast<int>(in[0] + 0.5f);
    int g = static_cast<int>(in[1] + 0.5f);
    int b = static_cast<int>(in[2] + 0.5f);
    if (r < 0) r = 0;
    if (r > 31) r = 31;
    if (g < 0) g = 0;
    if (g > 63) g = 63;
    if (b < 0) b = 0;
    if (b > 31) b = 31;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}
static uint16_t scheme_stop(int idx) { return scheme_pack(g_scheme_cur.ch[idx]); }

// The face's logical state + its state word. Accent-bearing elements
// (word, ring, meter, glow) follow this and nothing else does.
static aiwatchos::MuseState muse_face_state(const char** word_out) {
    if (g_state.listening) {
        *word_out = "LISTENING";
        return aiwatchos::MuseState::kListening;
    }
    if (g_speech_playing.load()) {
        // M10: SPEAKING is driven by real TTS playback, not a code.
        *word_out = "SPEAKING";
        return aiwatchos::MuseState::kSpeaking;
    }
    if (muse_outbox_depth() > 0 || g_state.turn_bytes > 0) {
        *word_out = "THINKING";
        return aiwatchos::MuseState::kThinking;
    }
#ifdef __ESPRESSIF_IDF__
    {
        // M2: a FRESH agent status (under 15 s — the companion expires
        // a pinned status after 5 s of event silence, but status
        // events pause while reply deltas flow) refines the face: an
        // error code wins, a speaking code confirms SPEAKING while
        // text streams, any other busy code holds THINKING.
        aiwatchos_net::MuseAgentStatus agent;
        if (aiwatchos_net::muse_link_agent_status(&agent) &&
            agent.age_ms >= 0 && agent.age_ms < 15000) {
            switch (activity_pose(agent.code)) {
                case ActivityPose::kError:
                    *word_out = "ERROR";
                    return aiwatchos::MuseState::kError;
                case ActivityPose::kSpeaking:
                    if (agent.busy ||
                        aiwatchos_net::muse_link_reply_streaming()) {
                        *word_out = "SPEAKING";
                        return aiwatchos::MuseState::kSpeaking;
                    }
                    break;
                case ActivityPose::kOff:
                case ActivityPose::kBoot:
                    *word_out = "LINKING";
                    return aiwatchos::MuseState::kBoot;
                case ActivityPose::kThinking:
                    if (agent.busy) {
                        *word_out = "THINKING";
                        return aiwatchos::MuseState::kThinking;
                    }
                    break;
                default:
                    break;   // idle/listening poses leave the face alone
            }
        }
    }
    switch (aiwatchos_net::muse_link_state()) {
        case aiwatchos_net::LinkState::kLinked:
            *word_out = "READY";
            return aiwatchos::MuseState::kReady;
        case aiwatchos_net::LinkState::kConnecting:
        case aiwatchos_net::LinkState::kWaitingWifi:
        case aiwatchos_net::LinkState::kBackoff:
            *word_out = "LINKING";
            return aiwatchos::MuseState::kBoot;
        case aiwatchos_net::LinkState::kOff:
            break;
    }
#endif
    *word_out = "READY";
    return aiwatchos::MuseState::kReady;
}

// Advance the cross-fade + animation clock. Called from muse_tick.
static void muse_face_anim(uint32_t elapsed_ms) {
    g_anim_ms += elapsed_ms;
    const char* word = nullptr;
    const aiwatchos::StateScheme& target =
        aiwatchos::state_scheme(muse_face_state(&word));
    const uint16_t stops[5] = {target.f0, target.f1, target.f2,
                               target.f3, target.accent};
    if (!g_scheme_init) {
        for (int i = 0; i < 5; ++i) scheme_unpack(stops[i], g_scheme_cur.ch[i]);
        g_scheme_init = true;
        return;
    }
    const float k = 1.0f - std::exp(-7.0f * static_cast<float>(elapsed_ms) / 1000.0f);
    for (int i = 0; i < 5; ++i) {
        float t[3];
        scheme_unpack(stops[i], t);
        for (int c = 0; c < 3; ++c) {
            g_scheme_cur.ch[i][c] += (t[c] - g_scheme_cur.ch[i][c]) * k;
        }
    }
}

// Turn buffer: heap-allocated (PSRAM on device) so 240 KB never lands in DRAM.
// Holds the current turn's ADPCM until the Noise transport drains it; freed
// when the next turn begins.
static uint8_t* g_turn_buf = nullptr;

// Offline outbox (companion outbox pattern): completed turns keep their ADPCM
// payloads until the transport drains them. Bounded: the oldest entry is
// dropped when full, so memory never grows without a link.
constexpr size_t kOutboxSize = 2;   // + the in-progress turn buffer
struct OutboxSlot {
    uint8_t* data = nullptr;
    size_t len = 0;
    uint64_t started_ms = 0;
    uint32_t duration_ms = 0;
};
OutboxSlot g_outbox[kOutboxSize] = {};
size_t g_outbox_count = 0;
bool g_flushing = false;

size_t muse_pending_bytes() {
    size_t total = g_state.turn_bytes;
    for (size_t i = 0; i < g_outbox_count; ++i) total += g_outbox[i].len;
    return total;
}

size_t muse_outbox_depth() { return g_outbox_count; }

bool muse_outbox_drain(bool (*send)(const uint8_t* data, size_t len, void* ctx),
                       void* ctx) {
    if (!send || g_flushing) return g_outbox_count == 0;
    g_flushing = true;
    while (g_outbox_count > 0) {
        OutboxSlot& head = g_outbox[0];
        if (!send(head.data, head.len, ctx)) break;   // stop, keep the rest
        free(head.data);
        for (size_t i = 1; i < g_outbox_count; ++i) g_outbox[i - 1] = g_outbox[i];
        g_outbox[g_outbox_count - 1] = OutboxSlot{};
        --g_outbox_count;
    }
    g_flushing = false;
    return g_outbox_count == 0;
}

// Activity log ring: the last kTurnLogSize completed turns, newest first.
MuseTurn g_turn_log[kTurnLogSize] = {};
size_t g_turn_total = 0;

namespace {
// Allocate the turn buffer: PSRAM on device, plain heap on host/tests.
uint8_t* turn_alloc() {
#ifdef __ESPRESSIF_IDF__
    return static_cast<uint8_t*>(
        heap_caps_malloc(kTurnMaxAdpcmBytes, MALLOC_CAP_SPIRAM));
#else
    return static_cast<uint8_t*>(malloc(kTurnMaxAdpcmBytes));
#endif
}

void turn_free() {
    free(g_turn_buf);   // heap_caps_malloc pairs with free
    g_turn_buf = nullptr;
}

// Begin a push-to-talk turn: fresh encoder, empty buffer, live capture.
bool begin_listen() {
    turn_free();
    g_turn_buf = turn_alloc();
    if (!g_turn_buf) return false;
    if (!g_hal || !g_hal->mic_start(16000)) {
        turn_free();
        return false;
    }
    g_state.encoder = muse_adpcm_t{};
    g_state.turn_bytes = 0;
    g_state.turn_start_ms = g_hal->now_ms();
    g_state.listening = true;
    return true;
}

void end_listen() {
    if (g_hal) g_hal->mic_stop();
    if (g_state.listening && g_hal) {
        const uint32_t dur =
            static_cast<uint32_t>(g_hal->now_ms() - g_state.turn_start_ms);
        // Log the completed turn for the activity log (newest first).
        MuseTurn done{g_state.turn_start_ms, dur, g_state.turn_bytes};
        for (size_t i = kTurnLogSize - 1; i > 0; --i) g_turn_log[i] = g_turn_log[i - 1];
        g_turn_log[0] = done;
        ++g_turn_total;
        // Hand the payload to the outbox (drop oldest when full); the
        // in-progress slot goes back to empty.
        if (g_outbox_count == kOutboxSize) {
            free(g_outbox[0].data);
            for (size_t i = 1; i < kOutboxSize; ++i) g_outbox[i - 1] = g_outbox[i];
            --g_outbox_count;
        }
        g_outbox[g_outbox_count++] =
            OutboxSlot{g_turn_buf, g_state.turn_bytes, g_state.turn_start_ms, dur};
        g_turn_buf = nullptr;
        g_state.turn_bytes = 0;
    }
    g_state.listening = false;
    g_state.mic_level = 0.0f;   // mic muted while idle (companion live-mode rule)
}
}  // namespace

float muse_mic_level() { return g_state.mic_level; }

float muse_progress() {
    if (!g_state.listening || kTurnMaxAdpcmBytes == 0) return 0.0f;
    float p = static_cast<float>(g_state.turn_bytes) /
              static_cast<float>(kTurnMaxAdpcmBytes);
    return p > 1.0f ? 1.0f : p;
}

size_t muse_turn_count() { return g_turn_total; }

bool muse_turn_at(size_t idx, MuseTurn* out) {
    if (!out || idx >= kTurnLogSize || idx >= g_turn_total) return false;
    *out = g_turn_log[idx];
    return true;
}

#ifdef __ESPRESSIF_IDF__
namespace {
// M10 playback seam for the net component's TTS engine: wraps
// muse_play_reply with the playing flag the face and barge-in read.
// The stop flag is cleared on entry — a stop only aborts the playback
// it was aimed at (stops during a fetch are the engine's generation
// check, not this flag's job).
size_t speech_player(const uint8_t* mp3, size_t len, void*) {
    g_speech_stop.store(false);
    g_speech_playing.store(true);
    const size_t played = muse_play_reply(mp3, len);
    g_speech_playing.store(false);
    return played;
}
}  // namespace
#endif

void muse_init() {
    if (g_state.initialized) return;

#ifdef __ESPRESSIF_IDF__
    // Transport wiring (device only): the net component's link task calls
    // this pump when a session is up; the pump drains the outbox FIFO
    // through the link's upload path. Offline/unconfigured, the pump is
    // simply never invoked and turns keep buffering, as before.
    aiwatchos_net::muse_link_set_outbox_pump(
        [](void*) {
            return muse_outbox_drain(
                [](const uint8_t* data, size_t len, void*) {
                    return aiwatchos_net::muse_link_upload_turn(data, len);
                },
                nullptr);
        },
        nullptr);
    // M10: register the MP3 player with the TTS engine.
    aiwatchos_net::muse_tts_set_player(&speech_player, nullptr);
#endif

    // The original muse_app_run() in main/app.c does:
    // 1. bsp_display_start() — CO5300 QSPI display init
    // 2. muse_pmu_init() — AXP2101 power management
    // 3. wifi_mgr_start() / connect to known network
    // 4. noise_control_init() — Noise protocol session with Muse server
    // 5. muse_voice_start() — ES8311/ES7210 audio codec init via I2S

    g_state.initialized = true;
    g_state.last_tick_ms = 0;
}

void muse_tick(uint32_t elapsed_ms) {
    if (!g_state.initialized) return;

    // Rate-limit: Muse's main loop runs at ~40ms per iteration (frame_ms=40).
    g_state.last_tick_ms += elapsed_ms;
    if (g_state.last_tick_ms < kTickRateMs) return;
    g_state.last_tick_ms = 0;
    muse_face_anim(elapsed_ms);

#ifdef __ESPRESSIF_IDF__
    {
        // M10: a reply that just SETTLED (final text published, no
        // longer streaming) is handed to the TTS engine exactly once:
        // cleaned with the companion cleaner at full length (no
        // 240-char display clip — the engine chunks). Detection state
        // lives in PSRAM; the engine itself gates on the Settings
        // toggle, the provisioned key, and the registered player.
        static char* tts_raw = nullptr;
        static char* tts_last = nullptr;
        if (!tts_raw) {
            tts_raw = static_cast<char*>(
                heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
        }
        if (!tts_last) {
            tts_last = static_cast<char*>(
                heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
            if (tts_last) tts_last[0] = '\0';
        }
        if (tts_raw && tts_last &&
            !aiwatchos_net::muse_link_reply_streaming() &&
            aiwatchos_net::muse_link_last_reply(tts_raw, 4096) &&
            tts_raw[0] && std::strcmp(tts_raw, tts_last) != 0) {
            std::snprintf(tts_last, 4096, "%s", tts_raw);
            const std::string cleaned = caption_clean_full(tts_raw);
            if (!cleaned.empty()) {
                aiwatchos_net::muse_tts_speak_text(cleaned.c_str());
            }
        }
    }
#endif

    // In the original code, muse_app_run() calls:
    // - muse_pmu_poll_key(): poll AXP2101 for BOOT/PWR button presses
    //   (BOOT triggers push-to-talk in hold mode)
    // - muse_link_loop(): process Noise protocol frames from the Muse server
    //   (handles prompt/display/notice/reply.delta message types)
    // - muse_voice_loop(): if listening, read ES7210 mic samples via I2S DMA,
    //   encode as ADPCM, and send over the encrypted WebSocket tunnel

    uint32_t buttons = g_hal ? g_hal->read_buttons() : 0;
    bool boot_pressed = (buttons & 0x01) != 0;  // bit 0 = BOOT

    // BOOT and touch are independent hold sources: either can start the turn,
    // and the turn ends only when BOTH are released (a touch-started turn must
    // not die because BOOT is up, and vice versa).
    if (boot_pressed && !g_state.boot_held) {
        g_state.boot_held = true;
        if (g_speech_playing.load()) {
            muse_speech_stop();   // M10 barge-in: BOOT stops the reply
        } else if (!g_state.listening) {
            // muse_voice_start(): enable ES7210 capture and encode as
            // ADPCM into the turn buffer; the Noise transport sends it
            // on release.
            begin_listen();
        }
    } else if (!boot_pressed && g_state.boot_held) {
        g_state.boot_held = false;
        if (!g_state.touch_held) end_listen();
    }
    if (g_state.listening && g_turn_buf) {
        // muse_voice_loop(): read mic frames, IMA-ADPCM encode (4 bits/sample),
        // buffer for the transport. Clamped at 30 s; the oldest audio is kept.
        int16_t pcm[256];
        size_t got = g_hal ? g_hal->mic_read(pcm, 256) : 0;
        // Live level meter: RMS amplitude 0..1 with fast attack, slow release
        // (mirrors the companion app's live-mode amplitude ballistics).
        if (got > 0) {
            double sum_sq = 0;
            for (size_t i = 0; i < got; ++i) {
                double s = static_cast<double>(pcm[i]) / 32768.0;
                sum_sq += s * s;
            }
            float instant = static_cast<float>(sqrt(sum_sq / got));
            if (instant > 1.0f) instant = 1.0f;
            if (instant >= g_state.mic_level) {
                g_state.mic_level = instant;                     // fast attack
            } else {
                g_state.mic_level += 0.2f * (instant - g_state.mic_level);  // release
            }
        }
        got &= ~1u;   // encoder needs even sample counts
        size_t room = g_state.turn_bytes < kTurnMaxAdpcmBytes
                          ? kTurnMaxAdpcmBytes - g_state.turn_bytes
                          : 0;
        size_t want = got / 2;
        if (want > room) want = room;
        if (want > 0) {
            muse_adpcm_encode_block(&g_state.encoder, pcm, want * 2,
                                    &g_turn_buf[g_state.turn_bytes]);
            g_state.turn_bytes += want;
        }
    }
}

namespace {

// Pixel-width word wrap for the caption (Caption face, <= 2 lines).
// Returns the line count; lines are written into `lines`.
int caption_wrap(const char* text, int max_w, char lines[][64], int max_lines) {
    using namespace aiwatchos;
    int n_lines = 0;
    const char* p = text;
    while (*p && n_lines < max_lines) {
        char* out = lines[n_lines];
        int out_len = 0;
        int width = 0;
        const char* word_start = p;
        while (*p && *p != '\n') {
            const char* w = p;
            while (*p && *p != ' ' && *p != '\n') ++p;
            const int word_len = static_cast<int>(p - w);
            char word_buf[64];
            const int copy_len = word_len < 63 ? word_len : 63;
            std::memcpy(word_buf, w, copy_len);
            word_buf[copy_len] = '\0';
            const int word_w = text_width_aa(word_buf, FontFace::kCaption);
            const int add = (out_len > 0 ? text_width_aa(" ", FontFace::kCaption) : 0) + word_w;
            if (out_len > 0 && width + add > max_w) {
                p = w;   // reprocess this word on the next line
                break;
            }
            if (out_len > 0) {
                out[out_len++] = ' ';
                width += text_width_aa(" ", FontFace::kCaption);
            }
            std::memcpy(out + out_len, word_buf, copy_len);
            out_len += copy_len;
            width += word_w;
            while (*p == ' ') ++p;
            word_start = p;
        }
        (void)word_start;
        out[out_len] = '\0';
        ++n_lines;
        if (*p == '\n') ++p;
    }
    return n_lines;
}

// The interim Jolly (plan 3.3 checkpoint): the locked character drawn
// from primitives in its FIXED palette — body, belly, face, dot eyes
// with a ~4.5 s blink, smile, blush, breathing bob. The muse_pixel.c
// sprite port is the follow-up that replaces this wholesale.
void draw_jolly(aiwatchos::Framebuffer& fb, int cx, int cy) {
    using namespace aiwatchos;
    const int bob = static_cast<int>(
        std::sin(static_cast<float>(g_anim_ms) * 6.28318f / 3000.0f) * 2.0f);
    const bool blink = (g_anim_ms % 4500) < 120;
    const int by = cy + bob;
    // Proportions are the locked character at 0.82 scale, centred
    // high enough that the meter row fits inside the stage below it.
    // Feet + arms (behind the body).
    fb.fill_circle(cx - 34, by + 79, 12, jolly::kFurMid);
    fb.fill_circle(cx + 34, by + 79, 12, jolly::kFurMid);
    fb.fill_circle(cx - 72, by + 25, 13, jolly::kFurMid);
    fb.fill_circle(cx + 72, by + 25, 13, jolly::kFurMid);
    // Body + belly.
    fb.fill_circle(cx, by + 13, 72, jolly::kFurMid);
    fb.fill_circle(cx, by + 36, 46, jolly::kFurLight);
    // Face: pale oval (two discs), lighter at the top.
    fb.fill_circle(cx, by - 23, 43, jolly::kSkin);
    fb.fill_circle(cx, by - 28, 33, jolly::kSkinLight);
    // Blush (pre-blended solid discs read as soft cheeks).
    const uint16_t blush =
        blend565(jolly::kSkin, jolly::kBlush, 90);
    fb.fill_circle(cx - 30, by - 11, 7, blush);
    fb.fill_circle(cx + 30, by - 11, 7, blush);
    // Eyes: dot eyes, or 3px lines mid-blink.
    if (blink) {
        fb.fill_round_rect(cx - 21, by - 27, 9, 3, 1, jolly::kOutline);
        fb.fill_round_rect(cx + 12, by - 27, 9, 3, 1, jolly::kOutline);
    } else {
        fb.fill_circle(cx - 16, by - 26, 4, jolly::kOutline);
        fb.fill_circle(cx + 16, by - 26, 4, jolly::kOutline);
    }
    // Smile: a shallow arc (ring segment through the bottom quadrant).
    fb.arc(cx, by - 13, 13, 2, 30.0f, 120.0f, jolly::kOutline);
}

}  // namespace

void muse_render(aiwatchos::Framebuffer& fb) {
    if (!g_state.initialized) return;
    using namespace aiwatchos;

    const int kCx = kDisplayWidth / 2;   // 205
    const int kCy = 232;                 // face centre (tokens section 6)
    constexpr int kRingR = 188;
    constexpr int kStageR = 138;

    const char* word = nullptr;
    const aiwatchos::MuseState face_state = muse_face_state(&word);
    const uint16_t accent = scheme_stop(4);
    if (!g_scheme_init) muse_face_anim(0);

    // State word (Headline, accent) at the top of the content zone.
    draw_text_aa_centered(fb, kCx, 50, word, FontFace::kHeadline, accent);
#ifdef __ESPRESSIF_IDF__
    {
        // M2: status line under the word — the task label when the
        // event carried one, else the companion caption for the
        // activity code. Shown while the status is fresh (60 s) or a
        // reply is still streaming in.
        aiwatchos_net::MuseAgentStatus agent;
        if (aiwatchos_net::muse_link_agent_status(&agent) &&
            agent.age_ms >= 0 &&
            (agent.age_ms < 60000 ||
             aiwatchos_net::muse_link_reply_streaming())) {
            std::string line = agent.label;
            if (line.empty()) activity_caption(agent.code, &line);
            if (!line.empty()) {
                draw_text_aa_centered(fb, kCx, 76, line.c_str(),
                                      FontFace::kCaption,
                                      agent.busy ? accent : colors::kDim);
            }
        }
    }
#endif

    // G6 glow: 8 rings between the stage and the bezel, scheme stops
    // f3 (outer) -> f0 (inner) in pairs.
    for (int i = 0; i < 8; ++i) {
        fb.ring(kCx, kCy, kStageR + 24 - 3 * i, 3, scheme_stop(3 - (i * 4) / 8));
    }

    // Bezel ring + G7 gloss arc (always on, state-independent).
    fb.ring(kCx, kCy, kRingR, 6, colors::kRingTrack);
    fb.arc(kCx, kCy, kRingR - 1, 2, 207.0f, 90.0f,
           blend565(colors::kRingTrack, colors::kWhite, 153));

    // State ring overlays: LISTENING progress (muse_progress of the
    // turn buffer), THINKING spinner (60 degrees at 300 deg/s).
    if (face_state == aiwatchos::MuseState::kListening) {
        fb.arc(kCx, kCy, kRingR, 6, 270.0f, muse_progress() * 360.0f,
               accent);
    } else if (face_state == aiwatchos::MuseState::kThinking) {
        const float spin =
            static_cast<float>(g_anim_ms % 1200) * 360.0f / 1200.0f;
        fb.arc(kCx, kCy, kRingR, 6, 270.0f + spin, 60.0f, accent);
    }

    // Stage disc: scheme f3 darkened toward black, faint top gloss.
    const uint16_t stage = lerp565(scheme_stop(3), colors::kBlack, 102);
    fb.fill_circle(kCx, kCy, kStageR, stage);
    fb.arc(kCx, kCy, kStageR - 8, 2, 200.0f, 110.0f,
           blend565(stage, colors::kWhite, 40));

    // Avatar (M19): the real muse_pixel renderer — a 256px blow-up of
    // its 64x64 grid centred where the interim avatar sat, background
    // cells transparent over the stage disc. Falls back to the interim
    // primitives if the PSRAM block could not be allocated.
    if (!g_pixel_tried) {
        g_pixel_tried = true;
        g_pixel_ok = muse_pixel_init();
        if (g_pixel_ok) muse_pixel_set_size(256);
    }
    if (g_pixel_ok) {
        const uint64_t now = g_hal ? g_hal->now_ms() : 0;
        if (face_state != g_pose_state) {
            g_pose_state = face_state;
            g_pose_since_ms = now;
        }
        muse_pose_t pose{};
        switch (face_state) {
            case aiwatchos::MuseState::kListening: pose.mode = MUSE_MODE_LISTENING; break;
            case aiwatchos::MuseState::kThinking:  pose.mode = MUSE_MODE_THINKING; break;
            case aiwatchos::MuseState::kSpeaking:  pose.mode = MUSE_MODE_SPEAKING; break;
            case aiwatchos::MuseState::kError:     pose.mode = MUSE_MODE_ERROR; break;
            case aiwatchos::MuseState::kBoot:      pose.mode = MUSE_MODE_BOOT; break;
            default:                               pose.mode = MUSE_MODE_IDLE; break;
        }
        pose.t = static_cast<float>(g_anim_ms) / 1000.0f;
        pose.mode_t = static_cast<float>(now - g_pose_since_ms) / 1000.0f;
        pose.level = g_state.listening ? g_state.mic_level : 0.0f;
        pose.happy = now < g_happy_until_ms
            ? static_cast<float>(g_happy_until_ms - now) / 1200.0f
            : 0.0f;
        muse_pixel_render(&pose);
        muse_pixel_scale(fb.pixels + (kCy - 14 - 128) * fb.width + (kCx - 128),
                         fb.width, 0, 255, 0, 255);
    } else {
        draw_jolly(fb, kCx, kCy - 14);
    }

    // Level meter INSIDE the stage (tokens: 14 segments 9x9 r3, gap 4):
    // lit = accent blended by the segment envelope, else faint.
    {
        const float env = g_state.listening ? g_state.mic_level : 0.0f;
        constexpr int kSegs = 14;
        constexpr int kSegW = 9, kSegGap = 4;
        const int x0 = kCx - (kSegs * (kSegW + kSegGap) - kSegGap) / 2;
        const int y = kCy + 96;   // inside the stage (half-width 92 there)
        for (int i = 0; i < kSegs; ++i) {
            const float shape =
                1.0f - std::fabs(static_cast<float>(i) - 6.5f) / 7.5f;
            float target = env * 1.6f * shape;
            if (target > 1.0f) target = 1.0f;
            g_seg[i] += (target - g_seg[i]) * 0.5f;
            const uint16_t c = lerp565(colors::kFaint, accent,
                                       static_cast<int>(g_seg[i] * 256.0f));
            fb.fill_round_rect(x0 + i * (kSegW + kSegGap), y, kSegW, kSegW,
                               3, c);
        }
    }

    // Caption bubble: the agent's reply — LIVE while it streams (M1),
    // the finished reply afterwards — cleaned with the companion's
    // rules (M3: markdown/fences stripped, clipped to 240) in PSRAM
    // scratch (4096 = the link's reply cap; never on the render stack).
    // Falls back to the latest-turn / hint text (host path included).
    char caption[256];
    caption[0] = '\0';
    const char* heard_text = "";   // M5: "You: <transcript>" (PSRAM, device)
#ifdef __ESPRESSIF_IDF__
    {
        static char* raw = nullptr;
        static char* cleaned_buf = nullptr;
        if (!raw) {
            raw = static_cast<char*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
        }
        if (!cleaned_buf) {
            cleaned_buf = static_cast<char*>(heap_caps_malloc(1024, MALLOC_CAP_SPIRAM));
        }
        if (raw && cleaned_buf) {
            const bool streaming = aiwatchos_net::muse_link_reply_streaming();
            bool have = streaming
                ? aiwatchos_net::muse_link_live_reply(raw, 4096)
                : aiwatchos_net::muse_link_last_reply(raw, 4096);
            if (!have && streaming) {
                have = aiwatchos_net::muse_link_last_reply(raw, 4096);
            }
            if (have && raw[0]) {
                std::string cleaned = caption_from_reply(raw);
                if (!cleaned.empty()) {
                    std::snprintf(cleaned_buf, 1024, "%s", cleaned.c_str());
                    std::snprintf(caption, sizeof(caption), "%s", cleaned_buf);
                }
            }
        }
    }
    {
        // M5: the latest heard transcript, prefixed for its bubble.
        static char* tbuf = nullptr;
        static char* ybuf = nullptr;
        if (!tbuf) {
            tbuf = static_cast<char*>(heap_caps_malloc(512, MALLOC_CAP_SPIRAM));
        }
        if (!ybuf) {
            ybuf = static_cast<char*>(heap_caps_malloc(576, MALLOC_CAP_SPIRAM));
        }
        if (tbuf && ybuf &&
            aiwatchos_net::muse_link_last_transcript(tbuf, 512) && tbuf[0]) {
            std::snprintf(ybuf, 576, "You: %s", tbuf);
            heard_text = ybuf;
        }
    }
#endif
    if (!caption[0]) {
        MuseTurn last = {};
        if (muse_turn_at(0, &last)) {
            std::snprintf(caption, sizeof(caption),
                          "LAST TURN %u.%us - %u KB",
                          static_cast<unsigned>(last.duration_ms / 1000),
                          static_cast<unsigned>((last.duration_ms % 1000) / 100),
                          static_cast<unsigned>(last.bytes / 1024));
        } else if (g_state.listening) {
            std::snprintf(caption, sizeof(caption), "Listening...");
        } else {
            std::snprintf(caption, sizeof(caption),
                          "Hold to talk - Muse answers here.");
        }
    }
    {
        char lines[2][64];
        const int n = caption_wrap(caption, 300, lines, 2);
        int text_w = 0;
        for (int i = 0; i < n; ++i) {
            const int w = text_width_aa(lines[i], FontFace::kCaption);
            if (w > text_w) text_w = w;
        }
        const int bw = text_w + 24;
        const int bh = n * font_line_height(FontFace::kCaption) + 12;
        const int bx = kCx - bw / 2;
        const int by = 374;
        // M5: heard-transcript bubble — one dim line directly above the
        // reply bubble ("You: ..." — what STT heard for this turn).
        if (heard_text[0]) {
            char tline[1][64];
            if (caption_wrap(heard_text, 300, tline, 1) > 0 && tline[0][0]) {
                const int tw = text_width_aa(tline[0], FontFace::kCaption);
                const int tbh = font_line_height(FontFace::kCaption) + 8;
                const int tby = by - tbh - 6;
                BubbleStyle tstyle = bubble_style();
                fb.draw_bubble(kCx - (tw + 20) / 2, tby, tw + 20, tbh, 12,
                               tstyle);
                draw_text_aa(fb, kCx - tw / 2, tby + 4, tline[0],
                             FontFace::kCaption, colors::kDim);
            }
        }
        // No sprite shadow: the baked card shadow is sized for the
        // 392px notification cards; the caption bubble is content-sized.
        BubbleStyle style = bubble_style();
        fb.draw_bubble(bx, by, bw, bh, 16, style);
        for (int i = 0; i < n; ++i) {
            draw_text_aa(fb, bx + 12,
                         by + 6 + i * font_line_height(FontFace::kCaption),
                         lines[i], FontFace::kCaption, colors::kCaption);
        }
    }

    // PTT disc (touch target unchanged: Press with y > H-100 starts,
    // Release ends — the shell now delivers real Release events).
    {
        BubbleStyle ptt = bubble_style_filled();
        fb.draw_bubble(kCx - 36, 424, 72, 72, 36, ptt);
        if (face_state == aiwatchos::MuseState::kListening) {
            fb.ring(kCx, 460, 40, 3, accent);
        }
        draw_icon(fb, kCx - 20, 440, Icon::kMic, FontFace::kIcon40,
                  colors::kMist);
    }
}

bool muse_on_touch(const aiwatchos::TouchEvent& event) {
    if (!g_state.initialized) return false;

    // Pet the avatar (M19): a tap on the character inside the stage
    // disc triggers muse_pixel's happy reaction (~1.2 s decay). The
    // stage (centre 205,232, r 138) never overlaps the PTT strip.
    if (event.type == aiwatchos::TouchEvent::Press) {
        const int dx = event.x - 205;
        const int dy = event.y - 232;
        if (dx * dx + dy * dy < 138 * 138) {
            if (g_speech_playing.load()) {
                muse_speech_stop();   // M10 barge-in: tap avatar = stop
                return true;          // consumed (not a pet)
            }
            g_happy_until_ms = (g_hal ? g_hal->now_ms() : 0) + 1200;
            return true;   // consumed
        }
    }

    // On this board, "touch" maps to BOOT/PWR via the touch panel. A press in
    // the bottom-center of the screen acts as push-to-talk (hold while speaking).
    if (event.type == aiwatchos::TouchEvent::Press &&
        event.y > aiwatchos::kDisplayHeight - 100) {
        if (g_speech_playing.load()) {
            muse_speech_stop();   // M10 barge-in: stop, don't record over it
            return true;          // consumed
        }
        g_state.touch_held = true;
        if (!g_state.listening) begin_listen();
        return true;   // consumed
    }

    if (event.type == aiwatchos::TouchEvent::Release && g_state.touch_held) {
        g_state.touch_held = false;
        // muse_voice_stop(): flush the final chunk; turn_bytes stays buffered
        // with its end-of-turn marker implied by the buffer length.
        if (!g_state.boot_held) end_listen();
        return true;
    }

    return false;  // not handled by muse
}

size_t muse_play_reply(const uint8_t* mp3, size_t len) {
    if (!g_hal || !mp3 || len == 0) return 0;
    if (!g_hal->audio_start_playback(16000)) return 0;

    mp3dec_t dec;
    mp3dec_init(&dec);
    // Decode + resample staging: PSRAM, allocated per call (M10 note).
    // These were function-local statics in the vendored player; while
    // the player had no caller the linker discarded them, but wiring
    // it up (M10) would have cost 6,912 B of static DIRAM. Both are
    // >4 KB-class buffers, so they follow the PSRAM rule instead —
    // speech is rare and long, the access pattern is sequential, and
    // allocation failure just skips the spoken reply (fail-closed).
#ifdef __ESPRESSIF_IDF__
    int16_t* frame = static_cast<int16_t*>(heap_caps_malloc(
        sizeof(int16_t) * MINIMP3_MAX_SAMPLES_PER_FRAME, MALLOC_CAP_SPIRAM));
    int16_t* out = static_cast<int16_t*>(
        heap_caps_malloc(sizeof(int16_t) * 1152, MALLOC_CAP_SPIRAM));
#else
    int16_t* frame = static_cast<int16_t*>(
        malloc(sizeof(int16_t) * MINIMP3_MAX_SAMPLES_PER_FRAME));
    int16_t* out = static_cast<int16_t*>(malloc(sizeof(int16_t) * 1152));
#endif
    if (!frame || !out) {
        free(frame);
        free(out);
        g_hal->audio_stop_playback();
        return 0;
    }
    size_t offset = 0, played = 0;
    // 16 kHz mono output staging: resample each decoded frame, then stream.
    // 1152-sample frames at up to 48 kHz shrink to <= 384 output frames.
    while (offset < len) {
        if (g_speech_stop.load()) break;   // M10 barge-in
        mp3dec_frame_info_t info = {};
        int samples = mp3dec_decode_frame(&dec, mp3 + offset, len - offset,
                                          frame, &info);
        if (info.frame_bytes <= 0) break;
        offset += info.frame_bytes;
        if (samples <= 0 || info.channels <= 0 || info.hz <= 0) continue;
        // Fold to mono, then linearly resample info.hz -> 16000 Hz.
        // out_n <= 1152 always (1152 in-frames shrink, never grow, to 16 kHz).
        int out_n =
            static_cast<int>((static_cast<int64_t>(samples) * 16000) / info.hz);
        // Sub-16 kHz sources would upsample past the staging buffer; clamp
        // (plays slightly fast) instead of overflowing it.
        if (out_n > 1152) out_n = 1152;
        if (out_n < 0) out_n = 0;
        for (int i = 0; i < out_n && i < 1152; ++i) {
            const int64_t num = static_cast<int64_t>(i) * info.hz;
            int j = static_cast<int>(num / 16000);          // input index
            if (j >= samples - 1) j = samples - 1;          // clamp tail
            if (j < 0) j = 0;
            const int rem = static_cast<int>(num % 16000);  // fraction to next
            int acc = 0;
            for (int c = 0; c < info.channels; ++c) acc += frame[j * info.channels + c];
            const int cur = acc / info.channels;
            int nxt = cur;
            if (j + 1 < samples) {
                int acc2 = 0;
                for (int c = 0; c < info.channels; ++c) acc2 += frame[(j + 1) * info.channels + c];
                nxt = acc2 / info.channels;
            }
            out[i] = static_cast<int16_t>(cur + ((nxt - cur) * rem) / 16000);
        }
        g_hal->audio_write(out, out_n > 1152 ? 1152 : out_n);
        played += static_cast<size_t>(out_n > 1152 ? 1152 : out_n);
    }
    free(frame);
    free(out);
    g_hal->audio_stop_playback();
    return played;
}

bool muse_speech_playing() { return g_speech_playing.load(); }

void muse_speech_stop() {
    g_speech_stop.store(true);
#ifdef __ESPRESSIF_IDF__
    aiwatchos_net::muse_tts_stop();   // drop pending text + later chunks
#endif
}

aiwatchos::App make_muse_app(aiwatchos::Hal& hal) {
    g_hal = &hal;
    return aiwatchos::make_app(
        "muse",
        muse_init,       // init
        muse_tick,       // tick
        muse_render,     // render
        muse_on_touch    // on_touch
    );
}

}  // namespace aiwatchos_muse
