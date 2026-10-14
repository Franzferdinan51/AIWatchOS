// Muse boot animation — plays once at startup, before the main loop.
//
// Staged ~2.2 s sequence on black: the baked "M" mark fades + rises in,
// "Muse" follows in Title white, then the Meta row (procedural
// lemniscate mark + caption), cycling progress dots, and a dim
// AIWatchOS credit. Any touch skips to the end. The sprite lives in
// PSRAM only while the animation plays; a missing sprite degrades to
// a text-only boot (never a stuck boot).
#pragma once

#include <cstdint>
#include "aiwatchos/app.hpp"
#include "aiwatchos/hal.hpp"

namespace aiwatchos {

// Total sequence length. render() clamps past it; the player exits at it.
inline constexpr uint32_t kBootAnimMs = 2200;

// Load the baked mark into PSRAM (false = fall back to text-only).
bool boot_anim_begin();
// Render the frame for t_ms since the sequence start (pure: same t
// renders the same pixels; safe to call without begin()).
void boot_anim_render(Framebuffer& fb, uint32_t t_ms);
// Release the baked mark.
void boot_anim_end();
// Blocking player for app_main: renders + flushes at ~30 fps until
// the sequence ends or a touch lands. Takes the Board (not the Hal)
// for row-range flushes.
void boot_anim_play(Board& board, Framebuffer& fb);

}  // namespace aiwatchos
