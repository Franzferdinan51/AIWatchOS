// AIWatchOS adapter for HermesGadget — wraps the hg::App conversation lifecycle
// as an OS-managed App with init/tick/render/input callbacks.
// Based on research of:
//   - firmware/core/include/hg/app.hpp (hg::App, DeviceProfile)
//   - firmware/core/include/hg/hal.hpp (Hal, Display, Transport interfaces)
//   - firmware/core/src/app.cpp (begin(), tick(), on_* event dispatch)
#pragma once

#include "aiwatchos/app.hpp"
#include "aiwatchos/hal.hpp"

namespace aiwatchos_hermes {

// Hermes app state — tracks the AI conversation lifecycle.
struct HermesState {
    bool initialized = false;
    // The hg::App instance is constructed during init and driven by tick().
    // It manages: WebSocket connection, Noise protocol handshake, push-to-talk
    // voice capture (ES7210), reply audio playback (ES8311), on-screen text display.
};

// Initialize the Hermes app: creates a DeviceProfile for the 2.06" board and
// constructs an hg::App with HAL callbacks wired to AIWatchOS drivers.
void hermes_init();

// Tick — called every ~40 ms by AppManager::tick(). Drives the hg::App state
// machine via App::tick(), which processes network I/O, audio streaming, and
// updates the UI model for rendering.
void hermes_tick(uint32_t elapsed_ms);

// Render — draws the Hermes conversation screen onto the framebuffer using the
// UiModel produced by tick(): shows reply text, status indicators, pairing codes,
// or error messages depending on the current Screen state.
void hermes_render(aiwatchos::Framebuffer& fb);

// Touch input handler — maps touch events to hg::App button callbacks:
// press bottom-center = TALK (push-to-talk), swipe down = CANCEL.
bool hermes_on_touch(const aiwatchos::TouchEvent& event);

// Build an OS App descriptor wrapping all callbacks above (App ID "hermes").
aiwatchos::App make_hermes_app();

}  // namespace aiwatchos_hermes
