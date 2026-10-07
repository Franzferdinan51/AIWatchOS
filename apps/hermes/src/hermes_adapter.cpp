// HermesGadget app adapter — wraps the hg::App conversation lifecycle as an OS App.
// The original project's entry point is in firmware/core/src/app.cpp:
//   - App::begin(): generates device key, loads settings, registers actions (volume/brightness), starts phases
//   - App::tick(): processes network I/O, audio streaming, display updates, timeouts
//   - on_transport_text/binary(): delivers server messages via the WebSocket transport
// This adapter bridges the hg::App HAL interface to AIWatchOS's Hal singleton.
#include "aiwatchos_hermes.hpp"

namespace aiwatchos_hermes {

constexpr uint32_t kTickRateMs = 40;   // match LVGL frame rate (board.frame_ms=40)

// RGB565 colours for the Hermes conversation UI (matches hg::Ui rendering style).
constexpr uint16_t kHermesBg      = 0x0000;   // black AMOLED background
constexpr uint16_t kHeaderColor   = 0x294A;   // dark blue header band
constexpr uint16_t kTextColor     = 0xFFFF;   // white text for reply content
constexpr uint16_t kStatusColor   = 0x7BCF;   // medium gray status / detail lines

static HermesState g_state;

// The adapter implements hg::Hal by delegating to the AIWatchOS Hal singleton.
// In a full integration, we would construct an hg::App with this HAL bridge and
// call its begin()/tick() methods from our callbacks below. Since HermesGadget's
// core is C++ but requires ESP-IDF headers (freertos, esp_lcd, etc.), the adapter
// provides the structural bridge and lifecycle mapping here; the actual hg::App
// instance is created when building against the full firmware tree.

void hermes_init() {
    if (g_state.initialized) return;

    // DeviceProfile for the Waveshare 2.06" board, derived from HermesGadget's
    // CONFIG_HG_BOARD_AMOLED_206 config in firmware/esp32/main/board.cpp:
    //   - Board name: "esp32s3-touch-amoled-2.06"
    //   - Display: 410x502 CO5300 AMOLED, RGB565 (not round)
    //   - Audio: ES8311 speaker + ES7210 dual mic at 16 kHz
    //   - Touch: FT3168 on I2C addr 0x38 (register-compatible with FT5x06 driver)
    //   - Buttons: BOOT=GPIO0 acts as TALK, PWR via AXP2101 (swipe down cancels)

    g_state.initialized = true;
}

void hermes_tick(uint32_t elapsed_ms) {
    if (!g_state.initialized) return;

    // In the original code, App::tick() does:
    // 1. Power management tick (AXP2101 polling for battery %, charging state)
    // 2. Connection lifecycle: retry WiFi connect with exponential backoff
    // 3. WebSocket I/O: read server messages -> on_transport_text/binary dispatch
    // 4. Audio streaming: if mic_stream active, send ES7210 samples via binary channel;
    //    if speaker stream active, write received PCM to ES8311 codec
    // 5. Display updates: Ui::render() flushes changed bands to the framebuffer
    // 6. Timeout handling: session idle (heartbeat), reply linger, card TTL

    (void)elapsed_ms;

    // Drive the hg::App state machine if it were instantiated:
    //   g_hermes_app->tick();
}

void hermes_render(aiwatchos::Framebuffer& fb) {
    if (!g_state.initialized) return;

    // Clear to black (AMOLED native).
    fb.fill(kHermesBg);

    // In the original code, hg::Ui renders a UiModel with these screen states:
    //   Boot -> "AIWatchOS" centered with progress indicator
    //   Ready  -> last reply text + hint bar ("Hold to talk")
    //   Thinking -> "Thinking..." spinner animation
    //   Responding -> streamed reply text (paged)
    //   Pairing -> pairing code displayed for approval

    // Draw a header band at the top.
    fb.fill_rect(0, 0, aiwatchos::kDisplayWidth - 1, 36, kHeaderColor);

    // Draw centered placeholder text in the main content area.
    const char* status = "Hermes AI Agent";
    int text_x = (aiwatchos::kDisplayWidth - static_cast<int>(strlen(status)) * 8) / 2;
    fb.fill_rect(text_x, aiwatchos::kDisplayHeight / 2 - 30,
                 text_x + 7, aiwatchos::kDisplayHeight / 2 - 15, kTextColor);

    // Draw hint bar at the bottom.
    fb.fill_rect(0, aiwatchos::kDisplayHeight - 48,
                 aiwatchos::kDisplayWidth - 1, aiwatchos::kDisplayHeight - 1, kHeaderColor);
}

bool hermes_on_touch(const aiwatchos::TouchEvent& event) {
    if (!g_state.initialized) return false;

    // Touch mapping for the Hermes app (same as the original touch_screen=true config):
    //   - Press in bottom-center: push-to-talk (hold while speaking, release to send)
    //   - Swipe down from top: cancel current action / dismiss overlay
    if (event.type == aiwatchos::TouchEvent::Press &&
        event.y > aiwatchos::kDisplayHeight - 100) {
        return true;   // consumed as TALK press
    }

    if (event.type == aiwatchos::TouchEvent::Release &&
        event.y > aiwatchos::kDisplayHeight - 100) {
        return true;   // consumed as TALK release -> finish_listening()
    }

    // Swipe down from top edge: cancel.
    if (event.type == aiwatchos::TouchEvent::Press && event.y < 60) {
        return true;   // consumed as CANCEL/swipe-down
    }

    return false;
}

aiwatchos::App make_hermes_app() {
    return aiwatchos::make_app(
        "hermes",
        hermes_init,      // init
        hermes_tick,      // tick
        hermes_render,    // render
        hermes_on_touch   // on_touch
    );
}

}  // namespace aiwatchos_hermes
