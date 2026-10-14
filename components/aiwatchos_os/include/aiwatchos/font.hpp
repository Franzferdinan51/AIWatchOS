// Anti-aliased text + icon rendering over the baked Inter assets
// (tools/gen_font.py -> assets/*.bin, DESIGN_TOKENS sections 5-6).
//
// The assets live in the aiwatchos_assets FAT partition on device and
// in the repo's assets/ directory on host; assets_load() reads them
// ONCE into PSRAM. Glyphs are blitted straight from the PSRAM-resident
// baked data (coverage blits are span-bounded and measured in
// microseconds); a 32-entry codepoint->record lookup cache (256 B
// static) keeps repeat lookups off the record walk. Far under the
// <=64 KB PSRAM cap and the UI SRAM budget.
//
// If the assets are unavailable (mount failure), every entry point
// falls back to the 5x7 debug font so the watch always has text; the
// fallback is a degraded mode, never the design.
#pragma once

#include <cstdint>
#include <string_view>

#include "aiwatchos/app.hpp"    // Framebuffer, AlphaSprite
#include "aiwatchos/icons.hpp"

namespace aiwatchos {

// Type scale (DESIGN_TOKENS section 5). Nominal px sizes are the bake
// sizes; line heights come from the baked face metrics.
enum class FontFace : uint8_t {
    kMicro,        // 15px SemiBold — status bar, labels, timestamps
    kCaption,      // 16px Regular — captions, notification body
    kBody,         // 18px Regular — primary reading text
    kBodySemibold, // 18px SemiBold — titles inside cards/lists
    kHeadline,     // 20px SemiBold — state word, screen titles
    kTitle,        // 28px SemiBold — hero secondary (weather condition)
    kPair,         // 40px Bold digits/upper — pairing codes
    kDisplay,      // 96px Bold digits — clock, weather hero numeral
    kIcon20,       // 20px icon face
    kIcon40,       // 40px icon face
    kIconWx64,     // 64px weather icon face
    kCount,
};

// Load all font + sprite assets into PSRAM. Idempotent. On device this
// mounts the aiwatchos_assets FAT partition read-only first. Returns
// false (and leaves the renderer in 5x7 fallback) if anything required
// is missing.
bool assets_load();
bool assets_ready();

// True when `face` was loaded (always true after a successful
// assets_load(); per-face so partial loads still degrade gracefully).
bool font_available(FontFace face);

// Face metrics for layout math. ascent = px above the baseline,
// line_height = full line box. In fallback these approximate the 5x7
// rendering (still safe for layout).
int font_ascent(FontFace face);
int font_line_height(FontFace face);

// Text width in px for UTF-8 text. Both overloads treat the string as
// a byte sequence decoded as UTF-8; unknown codepoints are skipped.
// tabular: digits were baked onto a fixed advance (gen_font.py), so
// times/vitals never jitter.
int text_width_aa(std::string_view utf8, FontFace face);

// Draw UTF-8 text with the pen at (x, y = TOP of the line box), in any
// RGB565 colour. Lowercase stays lowercase (the 5x7 font's fold was a
// defect, audit M1). Blending is confined to glyph pixels.
void draw_text_aa(Framebuffer& fb, int x, int y, std::string_view utf8,
                  FontFace face, uint16_t color);

// Convenience alignments (same y semantics).
void draw_text_aa_centered(Framebuffer& fb, int cx, int y,
                           std::string_view utf8, FontFace face,
                           uint16_t color);
void draw_text_aa_right(Framebuffer& fb, int x_right, int y,
                        std::string_view utf8, FontFace face,
                        uint16_t color);

// Draw one icon glyph with its em box top-left at (x, y). Pick the
// face by size: kIcon20 (settings chips, inline), kIcon40 (launcher
// tiles, card chips), kIconWx64 (weather hero; weather glyphs only).
void draw_icon(Framebuffer& fb, int x, int y, Icon icon, FontFace face,
               uint16_t color);

// R1 shared pixel wrap: greedy word wrap into at most max_lines lines
// (negative = unbounded). Overlong words hard-break; content cut by the
// line cap ends the last line with "...". Single source for every app —
// no bespoke wrap loops.
std::vector<std::string> wrap_text_aa(std::string_view text, FontFace face,
                                      int max_w, int max_lines);
// Truncate one line to max_w px with a trailing "..." (no-op when it fits).
std::string truncate_text_aa(std::string_view text, FontFace face, int max_w);

// Shadow sprites for the bubble recipe (G4), or nullptr when assets
// are not loaded. Tile = 96x96 tiles; pill = small pills/chips; card =
// variable-height cards (blit via blit_alpha4_vstretch with the caps
// returned by card_shadow_caps()).
const AlphaSprite* shadow_tile();
const AlphaSprite* shadow_pill();
const AlphaSprite* shadow_card();
void card_shadow_caps(int& cap_top, int& cap_bot);

}  // namespace aiwatchos
