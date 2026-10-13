# Implementation Plan — Watch UI/UX ("Muse Gloss" design system)

Spec: `DESIGN_TOKENS.md`. Findings: `UI_UX_AUDIT.md` (same directory).
Ordered: each phase unblocks the next. File paths are exact, from the
2026-10-13 source read. No phase may start by breaking a running
workstream's tree — AIWatchOS items land on the integration branch,
not on the feature branches in flight.

## Resource budget (binding on every phase)

Platform: ESP32-S3 — **512KB SRAM**, 8MB PSRAM, 32MB flash, 2MB app
slots; integrated firmware ~1.14MB before UI assets.

| Item | Cap | Enforcement |
|---|---|---|
| Font + icon + shadow assets per firmware image | **≤ 256KB flash** (plan ~170KB) | `tools/gen_font.py` prints totals, fails > cap; CI size check on the assets image |
| UI SRAM | **≤ 4KB** (gradient LUT 1,004B + glow LUT 16B + 2-line staging 1,640B + slack) | code review + link map check (`idf.py size`) on the integration branch |
| Glyph/text cache | **≤ 64KB, PSRAM only** (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`) | no static/global framebuffer-sized arrays; grep gate for `static uint16_t.*\[.*410` |
| Frame buffers | **1** framebuffer (411,640B PSRAM, already allocated) + existing 20-row DMA bounce | transitions = brightness-dip (Phase 3.0); nobody allocates a second frame |
| Per-frame blend work | text pixels + gloss bands (≤3700px) + shadow blits only | gradients are LUT row fills; no per-pixel lerp in fills |

Why safe: the UI's total new SRAM ask (4KB) is under 1% of SRAM; its
flash ask fits the existing ~5MB assets partition without touching the
2MB app slot (~860KB free there is NOT spent); its PSRAM ask (≤64KB
cache) is under 1% of PSRAM next to the existing 412KB framebuffer.
Other workstreams' budgets are untouched.

---

## Phase 0 — See the truth (before any restyle)

0.1 **Color/swap verification** (audit X8). Add a debug screen to
AIWatchOS (long-press BOOT on the clock face) drawing the HermesGadget
color-test pattern (pure R/G/B/W/black bands — `hg` ui.cpp
`m.color_test` is the reference) + the 7 pose accents + `bubble`
#14305A. On the physical panel: photograph, compare against the token
hexes. Decide the ONE swap point: if colors are rotated, the fix goes
in `components/aiwatchos_os/src/display_driver.cpp::co5300_flush_rows`
(the memcpy), never in app constants.
0.2 **Commit a host capture harness** for AIWatchOS:
`tools/host_shots/` — build the host target, render each registered
app into the host framebuffer, dump PNGs (the font pass did this
ad hoc; make it permanent). Output checked into the PR description,
not the repo. Same for muse-gadget-206: document the
`tools/muse/snap.py` flow in the repo README (device-tethered).

## Phase 1 — Framebuffer primitives (AIWatchOS)

All in `components/aiwatchos_os/include/aiwatchos/app.hpp` +
`src/app.cpp`. **Port, don't invent**: `hg::Canvas`
(HermesGadget `firmware/core/src/canvas.cpp`) already implements most
of this against the same panel class.

1.1 `rgb565(r,g,b)` constexpr helper + a `colors.hpp` token header
(every token from DESIGN_TOKENS section 1/3/9 as named constants).
Sweep: replace raw hex in `clock_face.cpp`, `launcher_ui.cpp`,
`settings_page.cpp`, `notifications.cpp`, `weather_app.cpp`,
`battery_display.cpp`, both app adapters. (Kills X8's accident palette.)
1.2 `blend565(dst, fg, alpha8)` inline; `put_pixel_blend`.
1.3 `fill_round_rect(x,y,w,h,r,color)` — scanline spans with corner
circle math (port of hg `fill_round_rect`).
1.4 `fill_circle`, `ring` (disc + inner cutout via spans), `arc`
(angular span fill — needed for the bezel spinner/listen arc; hg
`arc` is ±55° sound waves only, so this one is new: per-scanline
angle test is too slow — precompute the arc into a 1-bit mask in
PSRAM once per (radius, thickness) and blit-mask it; mask ≤ 402x402
bits = 20KB PSRAM, inside the cache cap).
1.5 `fill_vgrad(x,y,w,h, lut)` — per-scanline: LUT of ≤502 RGB565
entries computed on palette change (1,004B SRAM); rows filled in 4px
bands. Plus `draw_bubble(x,y,w,h,r, stops)` = fill_vgrad clipped to
round rect + G2 gloss + G3 highlight + G4 shadow blit (composes
1.2-1.5 + the build-time shadow sprites from Phase 2.3).
1.6 **Band flushing in the shell**: `main/app_main.cpp` stops flushing
`[0, 502)` every frame. Adopt HermesGadget's model: hash 4 horizontal
bands (FNV over the band's pixels, ui.cpp `Ui::render` pattern), flush
only changed bands via the existing `board_display_flush(fb, y0, y1)`
HAL range parameter. Fallback: dirty-rect union if hashing proves
costly (measure first).
1.7 **Touch event completion** (audit X6 — UI-blocking, owned here
because the renderers depend on it): `app_main`'s touch poll must
synthesize Release (press seen last frame, absent now) and Move
(position delta while pressed) from the FT3168 point stream. Without
this, Phase 3.4/3.7 interactions cannot work on device.
Unit tests: extend `components/aiwatchos_os/test/` — round-rect corner
pixels, blend endpoints (a=0 -> dst, a=255 -> fg), LUT monotonicity,
band-hash change detection. All 24+ existing host tests stay green.

**Perf note:** full-screen `fill()` today is 205,820 px writes/frame;
Phase 1 keeps fills as row spans (memset-class) and confines blends to
text + gloss + shadows (<5% of pixels). The flush change (1.6) is the
real perf item: full-frame QSPI ≈ ~80ms at 40MHz-class vs 40ms budget.

## Phase 2 — Font + icon pipeline

2.1 `tools/gen_font.py` (new, runs on the Mac; Pillow 11.3.0 already
installed — `PIL.ImageFont` over FreeType): inputs = Inter (OFL)
Regular/SemiBold/Bold TTFs; outputs = one `.h` (or assets `.bin`) per
size with 4-bit packed coverage + metrics; charset per DESIGN_TOKENS
section 6 incl. PUA icons; **subsets**: 40px = digits+A-Z, 96px =
`0-9 : . A P M °`. Prints per-file + total bytes; **fails if total
font+icon+shadow > 256KB**.
2.2 Renderer: `Framebuffer::draw_text_aa(x, y, text, font, color)` +
`text_width_aa` in `src/app.cpp` — per-glyph `blend565` over coverage;
lowercase stays lowercase (delete the fold); unknown glyph = nothing
(not a hollow box). Keep `draw_text` (5x7) ONLY behind
`AIWATCHOS_DEBUG_FONT` for boot logs.
2.3 Shadow sprites: extend the generator — render the 3 bubble shadows
(tile/card/pill) as 4-bit alpha PNG->bin sprites (blur done by PIL on
the Mac). Into the same assets image + budget.
2.4 Icon glyphs: Lucide/Feather SVG paths rasterized by the same
script into the PUA range of the 18/20/28 atlases (30 glyphs,
DESIGN_TOKENS section 7). One pipeline for type + icons = one budget.
2.5 LVGL fonts (muse-gadget-206): `lv_font_conv` Montserrat
16/20/28, 4bpp, same charset, into
`esp32/components/muse/fonts/`; sizes recorded; CJK font stays opt-in
(it is 850KB — it does NOT fit this budget and stays a config flag,
default off, as today).

## Phase 3 — AIWatchOS screen restyles (in this order)

Order rationale: shell first (it's on every screen), then the two
screens users judge the watch by (launcher, clock), then the flagship
(muse), then the rest.

3.0 **Shell** (`main/app_main.cpp`, `battery_display.cpp`): apps get a
content rect starting at y=44 and may not paint above it (fixes the
double-draw: delete the blank 36px bands in settings/notifications/
weather/hermes renders); status bar becomes transparent-over-backdrop:
time in Micro AA (delete the private 3x5 micro-font), battery per
tokens section 8 with % text + state colors; backdrop = G5 scanline
gradient in the shell's clear step; app switching via brightness-dip
(CO5300 brightness register ramp in `hal.cpp::set_backlight` path,
120ms down / switch / 120ms up).
3.1 **Launcher** (`launcher_ui.cpp`): 96x96 bubble tiles (draw_bubble
+ per-app foot tint + Hermes tile gold), 44px PUA glyph, Label under
glyph, press scale via brightness overlay 120ms; hit grid recomputed
to the token geometry; edge-swipe app-switching REMOVED from app
screens (launcher reached by swipe-up from clock / BOOT long-press —
decide in 3.0 and document).
3.2 **Clock face** (`clock_face.cpp`): digital-first — Display 96
tabular time (subset atlas), Caption date line, link/battery dot row;
analog variant rebuilt only if kept: ring track + 2x8 rounded ticks,
3px triangle hands, center disc r6 `blue`, second hand `blue` (delete
#FF0000). Delete the square clear + square center dot.
3.3 **Muse face** (`apps/muse/src/muse_adapter.cpp` render only):
stage composition per tokens 5/8 — bezel ring (Phase 1.4 arc:
track + accent indicator + G7 gloss arc), pixel avatar (port the
`muse_pixel.c` renderer — it is the same 64-grid art the companion
mirrors; it renders into the framebuffer as a bitmap blit with the
FIXED Jolly palette), state word in accent (Headline AA), caption
bubble in `caption` (the reply text — wire `muse_turn_at()`'s log
to it), 14-seg meter with per-segment level history (ballistics
already correct in the adapter), 76px PTT disc (visible target for
the bottom-strip touch logic), THINKING spinner + SPEAKING states
visualized (today: nonexistent). Interim checkpoint: everything
except the avatar port ships first (state word + ring + caption +
meter + disc = NEAR); avatar port completes it.
3.4 **Settings** (`settings_page.cpp`): bubble rows 64px with icon
chips + chevrons; brightness/volume = bubble sliders (thumb 28px);
DND = switch widget (44x24); About = real row; **delete the fake
Theme row and the misaligned scroll dots**; values become numbers
(brightness %, volume %) — fix the value-string logic that shows ON
for 50 and 100 alike.
3.5 **Notifications** (`notifications.cpp`): FIRST fix the wrap loop
(accumulate words to line width — port `hg::wrap_text` logic), then
the card list (icon chip, title, body, relative time from the
already-stored timestamp), newest first, swipe-right dismiss (needs
Phase 1.7 Move events), tap to expand; empty state = bell glyph in a
bubble + dim text.
3.6 **Weather** (`weather_app.cpp`): Display-size temp with real `°`
(subset atlas has it), 64px condition glyph (PUA, tinted by
condition), location Label, hi/lo + humidity + wind 3-cell row,
6-bubble hourly strip. (Data source is the weather workstream's
problem; render against its struct as-is.)
3.7 **Hermes stub** (`apps/hermes/src/hermes_adapter.cpp` render
only): Hermes-variant tokens — gold Title wordmark, health as Label
rows (battery with section-8 geometry, uptime, link state with dot),
hint text actually rendered in the bottom zone, delete the white
square "charging marker" and the duplicate battery bar. Functional
Hermes wiring is NOT this plan's scope.

## Phase 4 — muse-gadget-206 LVGL style pass

LVGL does gradients/shadows natively — this phase is style
application, not primitives. All in `esp32/components/muse/`.

4.1 **Theme object** (`muse_theme.c/.h`, new): the token table as
LVGL styles — `style_bubble` (bg gradient blueTop-family stops via
`lv_grad_dsc`, border white opa 56, radius 22, shadow blue/18/(0,8)),
`style_bubble_filled`, `style_card`, `style_row`, gloss child-object
helper (thin top object, white->transparent vertical gradient).
Init once; objects reference styles (LVGL styles are shared — RAM
cost is the style structs, ~100B each, SRAM-fine).
4.2 **Fonts**: swap chrome fonts per 2.5 — state word Montserrat 20,
caption Montserrat 16, power/name Montserrat 14 (delete unscii from
`muse_ui.c` labels; keep unscii ONLY if a debug screen needs it).
4.3 **Watchface** (`muse_ui.c`): bezel arc track -> `ringTrack`;
indicator stays pose accent; ADD the G7 gloss arc (second arc
object, white opa 153, width 2, 207-297°) + blue glow disc behind
the avatar (circle, `blue` opa 0x55, radius stage+8); meter segments
radius 3; speaker button -> `style_bubble_filled` + mic glyph stays
Montserrat symbol.
4.4 **Card + status** (`muse_status.c`): card -> `style_bubble` at
r-dialog, SET the body font explicitly (Montserrat 16 — today unset),
title Montserrat 20; status label unchanged except font token.
4.5 **Settings** (`muse_settings_ui.c`): `card()` style ->
`style_bubble` (pressed = gradient foot), ROW_H 58 -> 64, values
`dim` (chrome accent only on icons/selected), ALL page titles
Montserrat (kill the unscii `font_pick` branch at line 206).
4.6 **Menu + pairing** (`muse_menu.c`, pairing panel in
`muse_ui.c`): menu rows r 8 -> 16, selected = `bubbleBot` + 2px
`blue` left tick; pairing panel -> `style_bubble` r-dialog, code in
boot accent #A9C0FF.
Risk: LVGL gradient + shadow on many objects costs draw time on the
tileview scroll — mitigate by applying shadows to ≤6 objects/screen
and gradients only to cards/panels (rows get flat `bubble` + gloss
child, which is a single small gradient object).

## Phase 5 — HermesGadget polish (identity preserved)

All in `firmware/core/` (shared core) — the ESP32 port inherits it.

5.1 **AA text path**: `hg::Canvas::text` gains a coverage-blend path
fed by the SAME `tools/gen_font.py` output format (sizes 16/20/28 +
40px wordmark subset); 5x7 retained for the color-test/debug screens
only. Budget ≈65KB (section 11 envelope applies per image).
5.2 **Prompt buttons**: `draw_hero` buttons get the gloss recipe
(Hermes tokens: G1 stops #3D434C -> #181F2A -> #2E2C27 on green/red
dim bases — implement as base fill + top-half white blend at alpha
0.10 + 1px highlight) and r-bubble.
5.3 `fit()` truncation `..` -> `…` (free with the new charset).
5.4 Indicator `!`/`?`: draw as strokes (2px line + filled dot) in
`draw_indicator` instead of font glyphs.
5.5 Scroll thumb minimum width 4px (`draw_content`).
5.6 Rectangular top bar: drop the `kBar` band, float title + dot +
label over bg (match the round variant's quietness).
5.7 OPTIONAL P3: 2-bit (4-level) mascot at the largest size for AA
edges — only if the size table stays in budget; the 1-bit mascot is
approved as-is.
NOT in scope: mascot redesign, palette changes, screen set changes —
Hermes's identity and structure are the reference, not the patient.

## Phase 6 — Verification gates (every phase)

- Host tests green (AIWatchOS suite from repo root; HermesGadget
  python + C++ suites; gadget host tests after its build).
- Host captures (Phase 0.2) attached for every touched screen —
  before/after pairs.
- Size gates: firmware `.bin` within the 2MB slot; assets image
  ≤256KB (generator report); `idf.py size` DRAM delta for UI ≤ 4KB.
- On-device (first power-on): Phase 0.1 color check photo; 25 FPS
  held with the muse face animating (band flush stats logged);
  brightness-dip transition smoothness; touch PTT start/stop by
  touch alone (Phase 1.7).

## Risk register

| Risk | Where | Mitigation |
|---|---|---|
| QSPI flush saturates the frame budget once screens get richer | AIWatchOS | Phase 1.6 band flushing lands BEFORE any restyle; measure with flush stats |
| PSRAM bandwidth: framebuffer writes + audio turn buffer + WiFi | AIWatchOS muse | blends confined to <5% of pixels; gradient = solid row fills; avatar blit is a bounded 300x300 region |
| Font assets grow past cap as charset creeps | all | generator hard-fails > 256KB; subsets for 40/96 are contractual |
| LVGL gradient/shadow draw cost on tileview scroll | gadget | shadows ≤6 objects/screen; rows flat + gloss child (Phase 4 note) |
| Byte-swap surprise on first power-on rotates every color | AIWatchOS | Phase 0.1 runs before restyle merges; fix owned by display_driver only |
| Touch Release/Move synthesis misfires (PTT stuck on) | AIWatchOS | Phase 1.7 ships with a timeout failsafe in the muse adapter (auto-end turn at the 15s cap — already the firmware's MAX_SECS semantics) |
| Inter licensing paperwork | fonts | Inter is SIL OFL 1.1 (embeddable, notice in repo); fallback Roboto (Apache 2.0) is a drop-in at the generator |
| 96px clock atlas subset blocks future clock faces (e.g. seconds ring text) | AIWatchOS | subset is documented in the generator config; adding glyphs re-runs the budget check |
