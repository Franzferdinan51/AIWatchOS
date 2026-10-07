// AIWatchOS adapter for muse-gadget-206 — wraps the Muse voice interaction app
// as an OS-managed App with init/tick/render/input lifecycle callbacks.
// Based on research of:
//   - esp32/components/muse/muse_app.c (muse_app_run entry point)
//   - esp32/components/muse/muse_voice.h (voice pipeline: muse_voice_start/stop)
#pragma once

#include "aiwatchos/app.hpp"
#include "aiwatchos/hal.hpp"

namespace aiwatchos_muse {

// Muse app state — tracks the voice interaction lifecycle.
struct MuseState {
    bool initialized = false;
    bool listening = false;     // push-to-talk active (BOOT button held)
    uint32_t last_tick_ms = 0;  // for rate limiting
};

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
aiwatchos::App make_muse_app();

}  // namespace aiwatchos_muse
