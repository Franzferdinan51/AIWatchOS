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
void request_frame_card(bool on);
bool display_asleep();
const char* current_app_id();

namespace detail {
// Once per main-loop frame, before the power policy sees the sample:
// applies a pending app switch, replaces the touch points with the
// next injected frame while an injection sequence is active, and ORs
// any injected button mask into this frame's buttons. Returns true if
// anything was applied (the perf-wave loop uses it as a redraw hint).
bool apply_frame(int* n_points, int* xs, int* ys, uint32_t* buttons);

// True while the safe-area calibration card is latched on (`mdb frame`).
// The main loop draws the card over every finished frame — after the
// app and the status zone, before the band flush — so the card reaches
// the glass intact instead of being erased by the next render.
bool frame_card_requested();

// Frame-pipeline performance counters (perf-wave F0 instrumentation).
// The main loop is the only writer; the console task reads them for
// `mdb perf` / `mdb health`. Debug-grade by design, like the rest of
// the MDB state: a torn 64-bit read on the console side is acceptable.
// Window fields (frames..flush_us_max) are zeroed by perf_reset();
// the switch fields are last-event records and survive resets.
struct PerfStats {
    uint32_t frames = 0;         // awake loop iterations in the window
    uint32_t renders = 0;        // iterations that composed a frame
    uint32_t flush_frames = 0;   // iterations that flushed >= 1 band
    uint64_t period_us_sum = 0;
    uint32_t period_us_max = 0;
    uint64_t render_us_sum = 0;
    uint32_t render_us_max = 0;
    uint64_t hash_us_sum = 0;
    uint32_t hash_us_max = 0;
    uint64_t flush_us_sum = 0;
    uint32_t flush_us_max = 0;
    uint32_t switch_ms_last = 0;  // switch detect -> transition complete
    uint32_t switch_ms_max = 0;
    uint32_t dark_ms_last = 0;    // masked-switch dark window (T1)
};
extern PerfStats g_perf;
void perf_reset();
}  // namespace detail

}  // namespace aiwatchos_mdb
