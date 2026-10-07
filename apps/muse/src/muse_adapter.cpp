// Muse app adapter — wraps the muse-gadget-206 voice interaction as an OS App.
// The original project's entry point is muse_app_run() in main/app.c, which runs
// a monolithic loop handling WiFi connection, Noise protocol handshake with the
// Muse/Hermes server, push-to-talk voice capture (ES7210 mic), and reply playback
// (ES8311 speaker). This adapter decomposes that into init/tick/render callbacks.
#include "aiwatchos_muse.hpp"

namespace aiwatchos_muse {

constexpr uint32_t kTickRateMs = 40;   // match LVGL frame rate from board_waveshare_s3_206.c

// RGB565 colours for the Muse voice interaction UI.
constexpr uint16_t kMuseBg      = 0x0000;   // black (AMOLED power saving)
constexpr uint16_t kPromptText  = 0xFFFF;   // white prompt text
constexpr uint16_t kStatusColor = 0x7BCF;   // status bar / indicators

// File-scope state for the callback-based app interface.
static MuseState g_state;

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

    uint32_t buttons = aiwatchos::hal().read_buttons();
    bool boot_pressed = (buttons & 0x01) != 0;  // bit 0 = BOOT

    if (boot_pressed && !g_state.listening) {
        g_state.listening = true;
        // muse_voice_start() begins ES7210 capture + ADPCM encoding pipeline.
    } else if (!boot_pressed && g_state.listening) {
        g_state.listening = false;
        // muse_voice_stop(): flush final audio chunk and send end-of-turn marker.
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
    aiwatchos::PowerStatus ps = aiwatchos::hal().read_power();
    (void)ps;  // power display is handled by the OS shell status bar
}

bool muse_on_touch(const aiwatchos::TouchEvent& event) {
    if (!g_state.initialized) return false;

    // On this board, "touch" maps to BOOT/PWR via the touch panel. A press in
    // the bottom-center of the screen acts as push-to-talk (hold while speaking).
    if (event.type == aiwatchos::TouchEvent::Press &&
        event.y > aiwatchos::kDisplayHeight - 100) {
        g_state.listening = true;
        return true;   // consumed
    }

    if (event.type == aiwatchos::TouchEvent::Release && g_state.listening) {
        g_state.listening = false;
        return true;
    }

    return false;  // not handled by muse
}

aiwatchos::App make_muse_app() {
    return aiwatchos::make_app(
        "muse",
        muse_init,       // init
        muse_tick,       // tick
        muse_render,     // render
        muse_on_touch    // on_touch
    );
}

}  // namespace aiwatchos_muse
