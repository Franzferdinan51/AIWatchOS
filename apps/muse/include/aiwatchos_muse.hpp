// AIWatchOS adapter for muse-gadget-206 — wraps the Muse voice interaction app
// as an OS-managed App with init/tick/render/input lifecycle callbacks.
// Based on research of:
//   - esp32/components/muse/muse_app.c (muse_app_run entry point)
//   - esp32/components/muse/muse_voice.h (voice pipeline: muse_voice_start/stop)
#pragma once

#include "aiwatchos/app.hpp"
#include "aiwatchos/hal.hpp"
#include "muse_adpcm.h"

namespace aiwatchos_muse {

// One push-to-talk turn holds 30 s of 16 kHz mono encoded at 4 bits/sample.
constexpr size_t kTurnMaxAdpcmBytes = 16000u * 30u / 2u;

// Muse app state — tracks the voice interaction lifecycle.
struct MuseState {
    bool initialized = false;
    bool listening = false;     // push-to-talk active (any source held)
    bool boot_held = false;     // BOOT button currently down
    bool touch_held = false;    // bottom-screen touch currently down
    uint32_t last_tick_ms = 0;  // for rate limiting
    muse_adpcm_t encoder = {};  // IMA-ADPCM encoder state for the live turn
    size_t turn_bytes = 0;      // ADPCM bytes buffered for the in-progress turn
};

// ADPCM bytes buffered for the current turn (0 when idle). The Noise
// transport drains this when it lands; the UI can show a recording level.
size_t muse_pending_bytes();

// Play an MP3 reply (e.g. a spoken response fetched by the transport):
// decodes via minimp3, resamples to the 16 kHz voice rate, and streams to
// the speaker. Returns 16 kHz mono frames written (0 when audio is down).
size_t muse_play_reply(const uint8_t* mp3, size_t len);

// Initialize the muse app: sets up WiFi, audio codec (ES8311/ES7210), and
// prepares the voice pipeline. Called once by the OS on first launch.
void muse_init();

// Tick — called every ~40 ms by AppManager::tick(). Advances the Muse state
// machine: handles button polling, audio buffer processing, and connection
// lifecycle (WiFi connect -> Noise protocol handshake -> Hermes/Muse server).
void muse_tick(uint32_t elapsed_ms);

// Render — draws the Muse UI onto the framebuffer. Shows a voice interaction
// status screen with a "HOLD TO TALK" prompt on the 410x502 AMOLED.
void muse_render(aiwatchos::Framebuffer& fb);

// Touch input handler — BOOT button press starts/stops push-to-talk.
bool muse_on_touch(const aiwatchos::TouchEvent& event);

// Build an OS App descriptor wrapping all callbacks above (App ID "muse").
// Takes the Hal the adapter drives (Board on device, a fake in tests) instead
// of hard-wiring the Board singleton, which would make host tests unable to
// exercise capture and real firmware unable to substitute a loopback Hal.
aiwatchos::App make_muse_app(aiwatchos::Hal& hal = aiwatchos::Board::instance());

}  // namespace aiwatchos_muse
