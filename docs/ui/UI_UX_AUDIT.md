# UI/UX Audit — Watch Stack (AIWatchOS · muse-gadget-206 · HermesGadget)

Date: 2026-10-13. Method: full source read of all three rendering stacks +
the Muse Companion App theme/stage (the design source of truth), plus
viewed renders where they exist. Bar: Apple Watch / Pixel Watch / Galaxy
Watch. Verdicts are per screen: **BELOW BAR** / **NEAR** / **AT BAR**.
Companion spec: `DESIGN_TOKENS.md` (same directory). Fix order:
`IMPLEMENTATION_PLAN.md`.

## 0. What can actually be seen today (render inventory)

| Stack | Viewable? | How |
|---|---|---|
| AIWatchOS | Host-only | The OS builds against a host HAL with a memory framebuffer; the 2026-10-13 font pass host-rendered all 7 screens and eyeballed them (legible post-font). **No capture harness is committed to the repo** — those renders were one-off. On-device color has NEVER been verified (see finding X8). |
| muse-gadget-206 | Tethered only | USB-serial snapshot stream (`muse_ui_request_snapshot()`, LVGL `lv_snapshot_take` RGB565) + host tool `esp32/tools/muse/snap.py`. Needs the physical watch + serial. No simulator. |
| HermesGadget | YES | Desktop simulator (`firmware/sim`) + committed screenshots in `docs/images/screen-*.png`. Ready + Listening renders were viewed for this audit (round-panel variant, 466x466). |

Consequence: two of three stacks are effectively developed blind. Committing
capture tooling is a P1 fix in the plan, not an afterthought.

## 1. Cross-cutting findings (the short version)

- **X1 — Typography is the ceiling.** AIWatchOS and HermesGadget render ALL
  text in a 5x7 dot-matrix font at integer scales (10x14px body text; the
  weather hero number is 20x28px blocks). muse-gadget-206 renders its two
  most-read strings (state word, captions) in unscii, a pixel font, while
  its settings use Montserrat — two typographic voices in one UI. No
  anti-aliasing exists anywhere on any watch. This alone puts every screen
  below the bar.
- **X2 — Charset holes.** The shared 5x7 covers A-Z 0-9 and `! % ' + , - . / : ?`.
  No `" # $ & ( ) ; = @` and no `°`. Unsupported glyphs draw as hollow
  boxes. Weather literally prints `22C`. Lowercase is folded to UPPERCASE,
  so case in source strings is a lie (AIWatchOS Hermes draws "Hermes AI
  Agent" -> renders `HERMES AI AGENT`).
- **X3 — No alpha, no curves in AIWatchOS.** The Framebuffer API is
  `fill / hline / vline / fill_rect / draw_text`. There is no rounded rect,
  no circle, no gradient, no blend — gloss is *physically impossible*
  today, and the code knows it: the launcher comment says "Icon
  placeholder (rounded square)" directly above a `fill_rect` call.
  HermesGadget's `hg::Canvas` already has `fill_round_rect / fill_circle /
  ring / arc / bitmap1 / blit` — the reference implementation is in-family.
- **X4 — Two brands, three dialects, zero shared tokens.** Companion =
  gloss blue (#1877F2). muse-gadget-206 = flat violet (#A77DFF accent,
  #1A1530 cards) with NO blue and NO gradient anywhere. AIWatchOS = grays
  that decode to accidents (X8). Only HermesGadget is internally
  consistent — in its own (correct, separate) indigo/gold.
- **X5 — Motion vacuum.** AIWatchOS app switches are hard cuts; nothing
  anywhere has a transition, a press state, or a scroll animation. The
  25 FPS loop exists but is used as a slideshow timer.
- **X6 — The touch event model breaks the interactions the UI promises.**
  `main/app_main.cpp` synthesizes ONLY `TouchEvent::Press` — Release and
  Move are never generated on device. Result: Muse's touch push-to-talk
  can start but never end by touch; Notifications' drag-scroll is dead
  code; Hermes' swipe-down cancel is unreachable. The renderers were
  written against events the OS never delivers.
- **X7 — Full-frame flush every frame.** AIWatchOS re-renders and flushes
  all 410x502 every 40ms through a 20-row DMA bounce buffer on a
  40MHz-class QSPI bus (~412KB/frame ≈ 80ms of wire time at 40MHz — over
  the frame budget before any drawing happens). HermesGadget instead
  hashes 4 horizontal bands and flushes only changed rows. Any animation
  work in AIWatchOS is blocked on adopting band/dirty flushing (the HAL
  already accepts a y0..y1 range; `app_main` just never uses it).
- **X8 — Colors nobody has ever seen.** AIWatchOS constants (natural
  RGB565 -> approx sRGB): `0x294A` ≈ #292852 (slate navy, fine), `0x18E3`
  ≈ #181C18 (near-black, called "dark gray"), `0x52AA` ≈ #525552 (mid
  gray, called "blue highlight" in Settings), `0x7BEF`/`0x7BCF` ≈ #7B7D7B
  (neutral gray), `0xBDEF` ≈ #BDBE7B — a khaki yellow-green used as
  "light gray" body text in two apps, `0xF800` = pure #FF0000 (clock
  second hand), `0xBFDF` ≈ #BDFAFF (minute hand — the one good accident).
  Worse: `app.hpp` documents the framebuffer as "byte-swapped big-endian
  for CO5300", but the flush path is a plain `memcpy` with no swap —
  either the panel component swaps internally or every non-symmetric
  color is channel-rotated on real hardware. Unverifiable until the
  panel is powered; P0 check in the plan (HermesGadget ships a built-in
  color-test screen pattern for exactly this; AIWatchOS has none).

---

## 2. AIWatchOS (~/aiwatchos-build, branch pre-arrival-fixes)

Framebuffer: 410x502 RGB565 in PSRAM, apps draw via the API in X3, OS shell
draws a status bar, then the app, then a full flush. Apps registered:
Clock (boot), Launcher, Muse, Hermes, Settings, Notifications, Weather.

### 2.1 Status bar — BELOW BAR
`components/aiwatchos_os/src/battery_display.cpp`, drawn by `app_main`.
- Filled 36px band in `0x294A` across the top of every screen: flat,
  opaque, dated. Apple Watch has no bar at all; the spec replaces it with
  floating text over the backdrop (tokens section 8).
- The clock uses a PRIVATE 3x5 micro-font at scale 4 (12x20px digits made
  of 4x4 blocks) — a second, chunkier type system that the font fix did
  not unify. Digits + colon only. Time format `H:MM` (no leading zero,
  no AM/PM, no seconds).
- Battery: 48x20 outline + proportional fill, all in the same gray
  `0x7BCF`; charging = fill solid, i.e. indistinguishable from 100%.
  No nub on the outline, no % text, no low-battery color.
- **Double-draw bug:** the shell draws this bar FIRST, then the app
  renders — and Settings, Notifications, Weather and Hermes each paint
  their own blank 36px band over y=0..36, erasing the time and battery
  on 4 of 7 screens. The band they paint contains nothing.
- Exact changes: shell owns y<44 exclusively (apps get a content rect);
  delete the per-app bands; one AA font (Micro 15 SemiBold tabular);
  transparent bar; battery per tokens section 8 with % text and state
  colors.

### 2.2 Clock face — BELOW BAR
`components/aiwatchos_os/src/clock_face.cpp`. Analog only, centered
(205, 290), radius 180.
- Hands are 1px Bresenham lines: hour #FFF, minute ≈#BDFAFF, second pure
  #FF0000 (the only saturated color in the OS, and it's the wrong brand —
  spec: second hand in `blue`, or drop it for a sweeping dot).
- Tick marks are drawn by stroking a full line to the rim then erasing
  the inner part with background color — visible hack, 1px, no numerals,
  no minute track, no bezel ring.
- Center "dot" is a 7x7 SQUARE (`fill_rect`). The face clears its area
  with a square `fill_rect` too, so the dial's corners are a black square
  on the black page — invisible today only because everything is black.
- The header comment promises "a digital time readout"; none is drawn
  anywhere on this screen (the status bar's micro-digits are the only
  digits). Center y=290 leaves dead space below.
- No date, no complications, no gloss, no blue.
- Exact changes: digital-first face per tokens (Display 96 Bold tabular
  `textNum`, date at Caption in `dim`, state/link dot row beneath);
  keep an analog variant only if rebuilt: 3px tapered hands (two
  triangles, not lines), ring track `ringTrack` + ticks as 2x8 rounded
  marks at 12/3/6/9 only, center disc r=6 in `blue`, second hand `blue`.

### 2.3 Launcher — BELOW BAR (worst screen in the stack)
`components/aiwatchos_os/src/launcher_ui.cpp`.
- Every app is an identical 64x64 flat gray SQUARE (`fill_rect`,
  `0x7BEF`). `kLabelColor` is defined and **never used** — there are no
  labels. Seven identical gray boxes; the user memorizes positions or
  guesses. The comment calls them "(rounded square)"; they are not rounded.
- 3-col grid, gap 54px (computed, uneven rhythm vs the 36px row gap),
  first row y=60. No page indicator (7 apps = 3 rows, fits, but no
  affordance), no press feedback, no icons, no names.
- Navigation hazard: ANY screen's left/right 30px edge press switches
  apps (shell fallback), including mid-gesture in other apps.
- Exact changes: tiles per tokens section 5/7 — 96x96 r-bubble gradient
  tiles with G2 gloss, 44px PUA glyph, Label-size name under the glyph,
  per-app foot tint (Hermes tile gold), press scale 0.96/120ms, page
  dots if a second page ever appears; remove edge-swipe switching from
  app screens (keep it ONLY as the launcher's own gesture) or replace
  with swipe-up-to-launcher.

### 2.4 Muse — BELOW BAR (flagship app, weakest screen)
`apps/muse/src/muse_adapter.cpp::muse_render`.
- The entire screen: one line of 10x14 dot-matrix text ("HOLD TO TALK" /
  "LISTENING" / "READY") centered at y≈221, plus a meter at the bottom.
  No avatar, no stage, no ring, no caption, no state color (prompt is
  always white; there are no THINKING or SPEAKING visuals at all, and
  replies/transcripts are never rendered — the turn log exists in code
  and is displayed nowhere).
- The "level meter" is 24 bars x 12px, ALL AT THE SAME HEIGHT (every bar
  uses the single `mic_level` value): a solid block that grows and
  shrinks uniformly, in flat gray-blue `0x7BCF`. It reads as a progress
  bar. The companion/gadget meter is 14 segments driven by level with
  per-segment behavior; the RMS ballistics in the adapter (fast attack,
  0.2 release) are actually companion-correct — only the rendering is
  wrong.
- Touch PTT targets the bottom 100px strip invisibly (no button drawn),
  and per X6 its Release never arrives on device.
- Exact changes: rebuild as THE face from tokens sections 3/5/8 —
  stage + pixel avatar (port of `muse_pixel.c` renderer; it already
  targets this exact panel class), blue bezel ring + G7 gloss arc,
  state word in accent, caption bubble in `caption`, 14-segment meter,
  visible 76px PTT disc. Interim (pre-avatar): state word + ring +
  caption bubble + fixed meter gets this screen to NEAR.

### 2.5 Hermes (in AIWatchOS) — BELOW BAR (and a stub)
`apps/hermes/src/hermes_adapter.cpp`. `hermes_tick()` is empty; the
hg::App is never instantiated. The screen is a title + a battery bar.
- "Hermes AI Agent" centered at scale 2 — rendered UPPERCASE by the
  font fold (X2), so even the title isn't what the code says.
- Battery widget: 200x20 gray outline + gray fill at y=120, plus a white
  16x20 SQUARE as the "charging marker". Duplicates the status bar's job
  with different geometry.
- A bottom 48px band (`0x294A`) is drawn with NOTHING in it (the "hint
  bar" has no hint). The health line (`BAT 85% UP 01:23 LINK OFFLINE`)
  exists as a function and is rendered nowhere.
- No gold, no Hermes identity, no link state visual beyond the (unshown)
  text. Zero of HermesGadget's 15-screen design language is present.
- Exact changes: apply Hermes-variant tokens (section 9): gold title at
  Title size, health as Label rows (battery/link/uptime) with the
  section-8 battery geometry, hint text actually in the hint bar, link
  dot + label in the status zone. Functional wiring is another
  workstream's scope; the UI must stop being a placeholder either way.

### 2.6 Settings — BELOW BAR
`components/aiwatchos_os/src/settings_page.cpp`.
- Rows: 72px tall flat rects (`0x18E3` ≈ #181C18 — nearly black on black),
  8px side margins, sharp corners, 8px gaps. Selected row = `0x52AA`
  (≈ #525552, a mid GRAY — the comment calls it "blue highlight"; there
  is no blue on this screen).
- Labels at scale 2 (10x14) left; values right in muted gray as words —
  `HIGH/MED/LOW`, `ON/MUTE` — instead of numbers or controls. The Volume
  value logic shows `ON` for both 50 and 100 (only 0 shows MUTE).
  Brightness, a continuous value, cycles 3 steps with no slider.
  Do Not Disturb shows text ON/OFF instead of a switch. Theme row is a
  fake setting (hardwired dark, "cycle to show UX" per the comment) —
  a control that lies is worse than no control: remove it or make it
  real.
- "Scroll indicator dots" on the right edge are 5x6 gray squares whose
  pitch (78px) doesn't match the row pitch (72px), so they drift out of
  alignment down the list — and all 5 items fit on screen, so the
  indicator is decoration for scrolling that doesn't exist.
- No icons, no chevrons, no press feedback.
- Exact changes: rows per tokens section 5 (64px, r-bubble, icon chip,
  value in `dim`, chevron); brightness + volume become bubble sliders
  (track `ringTrack`, fill `blue`, thumb 28px disc with G2 gloss); DND
  becomes a switch (44x24 pill, knob 18px, on = `blue`); About becomes
  a real row (model, version, battery %); delete the fake Theme row
  (or ship the light theme — AMOLED says delete); delete the dots.

### 2.7 Notifications — BELOW BAR (layout is functionally broken)
`components/aiwatchos_os/src/notifications.cpp`.
- Only the LATEST notification is ever shown. No list, no app icon, no
  timestamp (the timestamp is stored and explicitly `(void)`-cast away
  in render), no unread count, no grouping.
- **The body renders ONE WORD PER LINE.** The wrap loop consumes a word,
  draws it, and advances `line_y` by 20px — "AIWatchOS started. Tap
  notifications to scroll." renders as six one-word lines down the
  screen. The code comment claims line wrapping to `chars_per_line`;
  the geometry disagrees.
- Title is hard-truncated at 31 chars (mid-word, no ellipsis glyph —
  the charset has none).
- Empty state is a bare centered "NO NOTIFICATIONS" — no glyph, no
  bubble.
- Scroll (X6) and swipe-to-dismiss (promised in the header comment)
  are unreachable/unimplemented respectively.
- Exact changes: card list per tokens section 8 (icon chip, title,
  wrapped body at Caption with REAL word wrap to the card width, time
  in `faint`), newest first, max 16 (already the store cap), tap =
  expand, swipe-right = dismiss; empty state = bell glyph in a bubble
  + "No notifications" at Body in `dim`; fix the wrap loop (accumulate
  words to `chars_per_line` per line — HermesGadget's `wrap_text` in
  `hg/canvas.hpp` is the in-family reference and does this correctly).

### 2.8 Weather — BELOW BAR
`components/aiwatchos_os/src/weather_app.cpp`. (Data is stubbed — another
workstream; this is the rendering.)
- Three lines of dot-matrix on a black void: temp at scale 4
  (20x28px blocks) — `22C`, no degree glyph exists (X2); condition at
  scale 2 in ≈#BDBE7B; humidity at the bottom in gray. A redundant
  black `fill_rect` "clear band" is drawn under the condition every
  frame.
- No weather icon (no icon system exists), no location, no hi/lo, no
  hourly strip, no wind — nothing that makes weather glanceable.
- Exact changes: hero temp at Display size (AA font makes 96px type
  possible) with a real `°`; condition glyph (PUA set, tokens section 7)
  at 64px in `warn`/`blue` by condition; location at Label in `dim`;
  hi/lo + humidity + wind as a 3-cell row of Label/Caption pairs;
  hourly strip = 6 mini bubbles. Keep the layout centered on the
  vertical axis — the current centering math is fine once type exists.

---

## 3. muse-gadget-206 (~/proj/muse-gadget-206, LVGL)

The richest watch UI of the three — and the wrong brand. LVGL gives it
real arcs, real layout, tileview navigation; everything is flat violet.
Face + caption-below layout genuinely mirrors the companion (by design —
the companion's PixelStage ports THIS firmware's numbers).

### 3.1 Watchface — NEAR (structure) / BELOW BAR (brand + type)
`esp32/components/muse/muse_ui.c` (1665 lines), `avatar/muse_pixel.c`.
- What works: pixel avatar (64-grid, full Jolly palette, per-mode glow
  schemes, blink/breathing/gaze life), bezel arc ring (diameter
  min(w,h)-8 = 402, 6px stroke, rotation 270°), state label, caption
  (256px, 2 lines), 14-seg meter (9x9, gap 4), status row (wifi/BLE
  symbols + power), speaker/mic controls, page dots, tileview settings.
  The thinking spinner (60° @ 300°/s), listen arc (15s fill), speaking
  (arc empty) all match the companion exactly.
- Brand: accent is idle-violet #A77DFF; ring track #140F22; there is NO
  #1877F2 anywhere and NO gradient, shadow, or gloss on any surface.
  The companion's stage has a blue bezel + white gloss arc + blue glow;
  the watch has none of the three. Per tokens rule 1 the bezel becomes
  blue-family chrome with the pose accent living in the indicator only.
- Type: state word + device name + power in **unscii** (8/16px pixel
  font, 2px letter-spacing on the state word); captions in unscii_16.
  Icons/status in Montserrat 14 symbols. So the face speaks pixel-type
  while settings (3.3) speak Montserrat — pick ONE voice: Montserrat AA
  (tokens section 6).
- The avatar canvas is a square whose black corners reach the ring
  (code comment admits it) — on the rectangular 410x502 panel the ring
  is a 402px circle floating in a taller screen; composition is
  inherited from the 466 round design (caption Y math literally
  special-cases a 466 circle). It reads fine, but the bottom ~90px is
  given to the status line (3.2) instead of a caption bubble.
- Pairing name label ("which gadget am I") under the state word is a
  thoughtful bench feature — keep, restyle in `dim` at Label size.
- Exact changes: LVGL style pass per plan Phase 4 — theme object with
  token colors; bezel: track `ringTrack`, gloss arc (LVGL arc, white
  opa 153, width 2, angles 207-297°); glow: blue 0x55 disc behind the
  stage (LVGL circle + opa) under the pose glow; state word/caption/
  power -> Montserrat 20/16/14 AA; meter segments radius 3; speaker
  button gets gradient + shadow styles.

### 3.2 Status line + card overlay — NEAR
`esp32/components/muse/muse_status.c`.
- Status line: Montserrat 16, 390px wide, centered, bottom — clean.
- Card: 360x300, radius 16, bg opa 95%, title Montserrat 20 centered,
  body font NOT set (inherits LVGL default) — an inconsistency hiding
  in plain sight; no card bg color set either (theme default), no
  border, no shadow, no gloss. TTL auto-dismiss + tap dismiss are right.
- Exact changes: card becomes a tokens bubble — r-dialog (24), G1
  gradient stops (LVGL gradient), 1px white-22% border, shadow (blue,
  18, (0,8)), gloss band (a thin child object with a white->transparent
  vertical gradient), title Title 28... on LVGL: Montserrat 28 may not
  be baked — plan bakes 16/20/28, use 20 SemiBold-equivalent (Montserrat
  20 + letter space) if 28 is too big for 360 wide; body Montserrat 16
  explicitly.

### 3.3 Settings UI — NEAR (best chrome in the stack)
`esp32/components/muse/muse_settings_ui.c` (1340 lines).
- Real components: cards (r=18, #1A1530, pressed #2E2552 — an actual
  press state, the only one in the stack), ROW_H 58, icon+label+value
  rows (Montserrat 20/20/16), sliders, switches, keyboard, pages for
  Wi-Fi join, sound/mic test, brightness, battery detail, about.
  This is the quality floor the other two should be held to.
- Gaps vs the bar: flat fills (no gradient/gloss/shadow), violet accent
  everywhere including values, row height 58 < tokens 64, value max
  width 130 truncates early, page titles in unscii_16 on some pages
  (the `font_pick` at line 206) vs Montserrat elsewhere.
- Exact changes: card style -> bubble recipe via LVGL styles (gradient
  + border + shadow + gloss child), accent -> `blue` for chrome and
  `text`/`dim` for values, ROW_H 64, all titles Montserrat.

### 3.4 Menu — NEAR
`esp32/components/muse/muse_menu.c` (628 lines). Button-driven fallback
(STATUS / BATTERY / POWER OFF / RESET PAIRING): rows r=8, selected
#2E2552, rule #2A2345, danger #FF5C5C (correct token already). Changes:
radius 8 -> 16, selected -> `bubbleBot` + `blue` left tick, same style
pass as 3.3. Low priority — it's a fallback UI.

### 3.5 Pairing screen — NEAR
Panel r=24 (already token-correct), bg #1A1530, border = accent;
title Montserrat 20 `text`, code Montserrat 28 in accent, hint
Montserrat 14 `dim`. Changes: bubble recipe (gradient/gloss/shadow),
code -> boot accent #A9C0FF (it already uses accent; pairing happens
in BOOT state so this mostly means don't let the accent drift),
spinner ring around the panel's icon area per tokens.

---

## 4. HermesGadget (/tmp clone of Franzferdinan51/HermesGadget @ bac7877)

**Keeps its indigo/gold identity** — audited for quality only.
`firmware/core/src/ui.cpp` (605 lines) over `hg::Canvas`.

### 4.1 What is already AT/NEAR BAR (say it plainly)
- **Rendering engineering is the best in the stack:** 4-band hash-based
  dirty flushing, hero mode that repaints only animated rows, clip
  rows, round-panel safe-area math. AIWatchOS should copy this (X7).
- **State indicator system:** 8-dot spinner with age falloff, 5-bar
  level icon, pulsing Ready disc (triangle-wave), error disc with `!`,
  pairing ring, prompt disc with `?` — a real iconographic language.
- **Hero composition (viewed):** 1-bit mascot portrait (pre-rendered at
  64/96/144/192+ sizes) centered with generous negative space; sound
  arcs at the ear (listening, green) and mouth (speaking, gold);
  thought dots ascending at the head corner (thinking, blue); caption
  headline in the STATE color, detail lines dim; link dot alone on
  round panels ("quiet like a watch's status mark" — correct instinct).
- **Text handling:** real word wrap (`wrap_text`), scroll with a
  thumb, `fit()` truncation, boot screen ("HERMES" in gold at scale+2,
  "gadget" dim beneath).
- Palette discipline: 14 named colors, semantic (green/blue/gold/red
  states, dim variants for button fills). Nothing accidental — the
  anti-AIWatchOS.

### 4.2 Polish gaps (ranked)
1. **The font.** Same 5x7 as everyone. In the viewed Ready render the
   mascot is genuinely beautiful line art and the caption under it —
   "Hi, I'm Hermes / Ask me anything" — has malformed, irregular
   glyphs (look at the `g`, the `y` descenders). The contrast between
   the art and the type is the whole critique. Fix = tokens section 6
   AA font at 16/20/28 (its `Canvas::text` is the only text path, so
   the swap is contained).
2. **Flat buttons.** Prompt YES/NO are flat dim fills (#1C5C3E /
   #7A2C2C) at r=3*scale — muddy at full opacity. Add the gloss recipe
   (Hermes tokens, section 9) + r-bubble.
3. **Truncation uses `..`** (two ASCII dots) instead of `…` — a charset
   artifact, dies with the font swap.
4. **Indicator glyphs `!` / `?`** are 5x7 text inside discs — at hero
   sizes they read as noise. Draw them as vector strokes (2px line +
   dot disc) in the canvas, or as PUA glyphs in the new atlas.
5. **Hairline scroll thumb** (1 scale-unit = 2px wide). Min 4px per
   tokens section 10.
6. **Top bar on rectangular panels** is a flat #181F2A band with a text
   link label (`NO NET/NET/LINK/ONLINE`). The label is honest and
   useful; restyle as floating text + dot like the round variant
   (drop the band), matching tokens section 8.
7. **Mascot is 1-bit** — a deliberate, coherent choice (pre-rendered
   sizes, blink/talk frames) and it looks GOOD. Do not "fix" it into
   color; the one upgrade worth making is a 2-bit (4-level) version at
   the largest size for AA edges, if flash allows — optional, P3.
8. Settings screens render as text rows through the same band system
   (functional, plain); after the font swap they inherit most of the
   lift. No structural change recommended.

### 4.3 AIWatchOS's Hermes stub vs this
Noted for the record: nothing from this design (mascot, indicators,
bands, gold) exists in AIWatchOS's Hermes app (2.5). If AIWatchOS is
to host Hermes, THIS ui.cpp is the reference implementation to port,
not a style to imitate from scratch.

---

## 5. Scoreboard

| Screen | Verdict | One-line reason |
|---|---|---|
| Companion app (reference) | AT BAR | the source of truth |
| AIWatchOS status bar | BELOW | filled band, private 3x5 font, double-drawn over |
| AIWatchOS clock | BELOW | 1px hands, square dot, red hand, no digits |
| AIWatchOS launcher | BELOW | identical unlabeled gray squares |
| AIWatchOS muse | BELOW | one line of block text + a uniform block meter |
| AIWatchOS hermes | BELOW | stub: title + battery bar + empty hint band |
| AIWatchOS settings | BELOW | near-black flat rows, lying values, fake theme row |
| AIWatchOS notifications | BELOW | one word per line; latest only; no time |
| AIWatchOS weather | BELOW | `22C` in 20x28 blocks on a black void |
| Gadget watchface | NEAR | great structure/life, wrong brand, pixel type |
| Gadget card/status | NEAR | clean, flat, body font unset |
| Gadget settings | NEAR | real components; flat violet |
| Gadget menu | NEAR | fine fallback; radius/accent dated |
| Gadget pairing | NEAR | right shape; needs the bubble recipe |
| HermesGadget (all 15) | NEAR | best engineering; font + flat buttons cap it |

Nothing on any watch is AT BAR today. The gap is not taste — every
defect above is a missing primitive (AA text, alpha blend, rounded
geometry, band flushing) or a missing hour of style application. The
primitives are specified in DESIGN_TOKENS.md and sequenced in
IMPLEMENTATION_PLAN.md.
