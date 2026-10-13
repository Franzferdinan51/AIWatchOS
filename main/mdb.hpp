// MDB — the Muse Developer Bridge. See docs/mdb.md for the protocol
// contract (the Mac-side CLI/MCP host half is built against that doc).
//
// Two halves live here:
//   start()          — the console task + log ring (mdb_console.cpp).
//                      Firmware-only, and only when
//                      CONFIG_AIWATCHOS_MDB_ENABLE; a no-op otherwise.
//   control hooks    — input-injection state, implemented in
//                      app_main.cpp next to the state it touches and
//                      consumed by the main loop at its normal input
//                      sampling point, so synthetic input is
//                      indistinguishable from physical input downstream
//                      (same TouchSynth, same DisplayPower, same
//                      AppManager dispatch — nothing is faked at the
//                      HAL or the app layer).
#pragma once

#include <cstdint>

namespace aiwatchos_mdb {

void start();

// --- control hooks (called by the console task) ----------------------
void inject_tap(int x, int y);
void inject_swipe(int x1, int y1, int x2, int y2, int duration_ms);
void inject_button(uint32_t mask);   // Board button mask: bit0 BOOT, bit1 PWR
void request_switch_app(const char* app_id);
void request_wake();
void request_sleep();
bool display_asleep();
const char* current_app_id();

namespace detail {
// Once per main-loop frame, before the power policy sees the sample:
// applies a pending app switch, replaces the touch points with the
// next injected frame while an injection sequence is active, and ORs
// any injected button mask into this frame's buttons.
void apply_frame(int* n_points, int* xs, int* ys, uint32_t* buttons);
}  // namespace detail

}  // namespace aiwatchos_mdb
