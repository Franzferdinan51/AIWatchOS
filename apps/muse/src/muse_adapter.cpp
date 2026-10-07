// Muse app adapter — wraps the muse-gadget-206 voice interaction as an OS App.
// The original project's entry point is muse_app_run() in main/app.c, which runs
// a monolithic loop handling WiFi connection, Noise protocol handshake with the
// Muse/Hermes server, push-to-talk voice capture (ES7210 mic), and reply playback
// (ES8311 speaker). This adapter decomposes that into init/tick/render callbacks.
#include "aiwatchos_muse.hpp"
#include "minimp3.h"
#include <cstdlib>
#include <cstring>

#ifdef __ESPRESSIF_IDF__
#include "esp_heap_caps.h"
#endif

namespace aiwatchos_muse {

constexpr uint32_t kTickRateMs = 40;   // match LVGL frame rate from board_waveshare_s3_206.c

// RGB565 colours for the Muse voice interaction UI.
constexpr uint16_t kMuseBg      = 0x0000;   // black (AMOLED power saving)
constexpr uint16_t kPromptText  = 0xFFFF;   // white prompt text
constexpr uint16_t kStatusColor = 0x7BCF;   // status bar / indicators

// File-scope state for the callback-based app interface.
static MuseState g_state;
// Hal driven by this adapter (Board on device, injected fake in tests).
static aiwatchos::Hal* g_hal = nullptr;
// Turn buffer: heap-allocated (PSRAM on device) so 240 KB never lands in DRAM.
// Holds the current turn's ADPCM until the Noise transport drains it; freed
// when the next turn begins.
static uint8_t* g_turn_buf = nullptr;

size_t muse_pending_bytes() { return g_state.turn_bytes; }

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
    g_state.listening = true;
    return true;
}

void end_listen() {
    if (g_hal) g_hal->mic_stop();
    g_state.listening = false;
    // turn_bytes stays buffered for the transport; freed at next begin_listen().
}
}  // namespace

void muse_init() {
    if (g_state.initialized) return;

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
        // muse_voice_start(): enable ES7210 capture and encode as ADPCM into
        // the turn buffer; the Noise transport sends it on release.
        if (!g_state.listening) begin_listen();
    } else if (!boot_pressed && g_state.boot_held) {
        g_state.boot_held = false;
        if (!g_state.touch_held) end_listen();
    }
    if (g_state.listening && g_turn_buf) {
        // muse_voice_loop(): read mic frames, IMA-ADPCM encode (4 bits/sample),
        // buffer for the transport. Clamped at 30 s; the oldest audio is kept.
        int16_t pcm[256];
        size_t got = g_hal ? g_hal->mic_read(pcm, 256) : 0;
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

void muse_render(aiwatchos::Framebuffer& fb) {
    if (!g_state.initialized) return;

    // Clear to black (AMOLED native — saves power).
    fb.fill(kMuseBg);

    // Draw a centered prompt: "HOLD TO TALK" while idle, or listening status.
    const char* prompt = g_state.listening ? "LISTENING..." : "HOLD TO TALK";
    int text_x = (aiwatchos::kDisplayWidth - static_cast<int>(strlen(prompt)) * 8) / 2;

    // Simple bitmap-style text rendering: draw each character as an 8x16 block.
    for (size_t i = 0; prompt[i]; ++i) {
        int cx = text_x + static_cast<int>(i) * 9;
        int cy = aiwatchos::kDisplayHeight / 2 - 30;
        // Draw a placeholder rectangle for each character.
        fb.fill_rect(cx, cy, cx + 7, cy + 15, kPromptText);
    }

    // Status bar at top showing battery and connection state.
    aiwatchos::PowerStatus ps = g_hal ? g_hal->read_power() : aiwatchos::PowerStatus{};
    (void)ps;  // power display is handled by the OS shell status bar
}

bool muse_on_touch(const aiwatchos::TouchEvent& event) {
    if (!g_state.initialized) return false;

    // On this board, "touch" maps to BOOT/PWR via the touch panel. A press in
    // the bottom-center of the screen acts as push-to-talk (hold while speaking).
    if (event.type == aiwatchos::TouchEvent::Press &&
        event.y > aiwatchos::kDisplayHeight - 100) {
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
    static int16_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t offset = 0, played = 0;
    // 16 kHz mono output staging: resample each decoded frame, then stream.
    // 1152-sample frames at up to 48 kHz shrink to <= 384 output frames.
    static int16_t out[1152];
    while (offset < len) {
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
    g_hal->audio_stop_playback();
    return played;
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
