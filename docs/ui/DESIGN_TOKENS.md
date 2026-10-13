# Watch Design System — "Muse Gloss" v1.0

One visual language for the whole watch stack, taken from the Muse Companion
App (`~/proj/MuseCompanionApp/app/lib/ui/muse_theme.dart` + `pixel_stage.dart`,
verified in source 2026-10-13) and projected onto a 410x502 RGB565 AMOLED
(CO5300) with cheap framebuffer/LVGL primitives.

Two dialects, **one geometry**: **Muse** (gloss blue) for AIWatchOS and
muse-gadget-206, **Hermes** (indigo/gold, section 9) for HermesGadget.
Everything else — radii, type scale, spacing, motion, gloss recipe — is shared.

Rules that outrank everything else in this file:

0. **Resource law (ESP32-S3: 512KB SRAM, 8MB PSRAM, 32MB flash, 2MB app
   slots, integrated firmware already ~1.14MB before UI assets).**
   - ALL font + icon + shadow assets in a firmware image: **≤ 256KB of
     flash, total** (sized in section 6; shadows in section 4-G4).
   - **No full-screen intermediate buffer in SRAM, ever.** All gradient/
     gloss/shadow math is per-scanline or per-band directly into the one
     framebuffer (which lives in PSRAM). Small LUTs only.
   - Any glyph/text cache lives in **PSRAM**, capped at 64KB.
   - Animation uses the existing framebuffer + DMA bounce buffer only —
     **no extra full-frame buffers** (transitions are brightness-dip,
     section 10).
   Full budget table: section 11.
1. **Chrome is always blue; state color is a spice, never a theme.** Bezel,
   bubbles, selection, links = the blue family in every state. The pose accent
   (section 3) may appear ONLY in: the state word, the ring progress/spinner,
   the level meter, the avatar glow, and an 8px status dot. Never tint a whole
   screen by state.
2. **No raw hex in app code.** Colors are named tokens. On the framebuffer
   they are produced by a compile-time `rgb565()` helper and byte-swapped at
   exactly ONE point — the display flush (the pattern HermesGadget's
   `hg::Canvas::encode()` already implements). AIWatchOS today has no swap in
   its flush path despite a header comment claiming swapped pixels; token
   values below are canonical (unswapped) RGB565.
3. **Nothing is a sharp-cornered flat rectangle.** Every surface is a bubble:
   gradient + gloss band + 1px inner highlight + soft shadow (section 4).

---

## 1. Color — Muse dialect (canonical tokens)

| Token | Hex | RGB565 | Source / use |
|---|---|---|---|
| `blue` | #1877F2 | 0x1BBE | museBlue. Primary: bezel ring, selection, links, chart accents |
| `blueDeep` | #0A3F86 | 0x09F0 | museBlueDeep. Gradient foot, pressed states |
| `blueTop` | #4B95F5 | 0x4CBE | Computed: lerp(blue, white, .22). Top stop of filled (own/primary) bubbles |
| `blueBot` | #1467D4 | 0x133A | Computed: lerp(blue, blueDeep, .28). Bottom stop of filled bubbles |
| `bubbleTop` | #3A5174 | 0x3A8E | Computed: lerp(bubble, white, .16). Top stop of surface bubbles (dark) |
| `bubble` | #14305A | 0x118B | Card dark in the Flutter theme. THE surface color |
| `bubbleBot` | #123465 | 0x11AC | Computed: lerp(bubble, blueDeep, .25). Bottom stop of surface bubbles |
| `ink` | #07101C | 0x0083 | museInk. Page background base |
| `night` | #0C1828 | 0x08C5 | museNight. App background over ink |
| `input` | #13233C | 0x1107 | Input/keyboard field fill (dark) |
| `ringTrack` | #0A2343 | 0x0908 | Spec: lerp(ink, blue, .18). Bezel/progress track (replaces gadget #140F22) |
| `text` | #F2EFFF | 0xF77F | Primary text (companion COLOR_LIT). Warm white, not #FFF |
| `textNum` | #FFFFFF | 0xFFFF | Headline numerals only (clock digits, pairing code) |
| `caption` | #D8D2FF | 0xDE9F | Caption/reply text under the avatar (COLOR_CAPTION, identical in Dart + C) |
| `dim` | #8B84A8 | 0x8C35 | Secondary text, inactive icons (COLOR_DIM) |
| `faint` | #3A3358 | 0x398B | Tertiary text, off-segments, page dots off (COLOR_DOT_OFF) |
| `ok` | #6FF0BF | 0x6F97 | Success / connected (= speaking accent) |
| `warn` | #FFB45C | 0xFDAB | Warning (gadget COLOR_WARN) |
| `danger` | #FF5C5C | 0xFAEB | Error / destructive (= error accent) |
| `mist` | #F4F8FF | 0xF7DF | museMist. Light surface; on the watch: text on filled blue bubbles |

Gloss/shadow constants (from `MuseBubble`, dark mode — the watch is dark-only):

- Gloss band: white at alpha **0.22** (56/255), fading to 0 over **10px**,
  inset **12px** left/right, starting **1px** below the top edge.
- Border: **1px white at alpha 0.22** on surface bubbles, **0.34** on filled
  blue bubbles, **0.18** on assistant/other bubbles (chat recipe).
- Shadow: color `blue`, alpha **0.28** (bubble) / **0.18** (chat bubble),
  blur **18px** (bubble) / **12px** (chat), offset **(0, +8)** / **(0, +6)**.
  On the framebuffer this is faked, not blurred — see recipes G4/G6.
- Avatar glow (companion stage): `blue` at alpha **0x55 (33%)**, blurred disc
  radius = stage radius + 8, blur sigma ~16. Logo glow: blue alpha 0.45,
  blur = 28% of size, y-offset = 8% of size.

## 2. The avatar (Jolly) fixed palette

From `muse_pixel.c` FIXED table — the character never changes color with state;
only its glow does.

| Token | Hex | RGB565 |
|---|---|---|
| furLight | #F8EEDC | 0xFF7B |
| furMid | #E6D7BD | 0xE6B7 |
| furDark | #AE987E | 0xACCF |
| skin | #F6DFBD | 0xF6F7 |
| skinLight | #FDEED6 | 0xFF7A |
| blush | #F4AAA0 | 0xF554 |
| heart | #FF4F8B | 0xFA71 |
| outline | #3A2B22 | 0x3944 |

## 3. State system (canonical — from muse_pixel.c SCHEMES, mirrored in Dart)

State words are the gadget's own: READY / LISTENING / THINKING / SPEAKING /
ERROR / BOOT / OFF, plus CONNECTING / RECONNECTING as READY sub-states.
Each state owns a 4-stop glow ramp (f0 brightest -> f3 deep) + one accent.
Palette cross-fades between states at expEase rate 7 (`1 - exp(-dt*7)`).

| State | f0 | f1 | f2 | f3 | Accent |
|---|---|---|---|---|---|
| BOOT | #FFFFFF 0xFFFF | #CFE0FF 0xCF1F | #8FA8FF 0x8D5F | #5A5FE0 0x5AFC | #A9C0FF 0xAE1F |
| READY (idle) | #F4E8FF 0xF75F | #C7A4FF 0xC53F | #9A6BFF 0x9B5F | #5B3FD9 0x59FB | #A77DFF 0xA3FF |
| LISTENING | #E8FAFF 0xEFDF | #8FDCFF 0x8EFF | #3FA2FF 0x3D1F | #2A5BD7 0x2ADA | #5CB8FF 0x5DDF |
| THINKING | #FFE6FF 0xFF3F | #FF9CF0 0xFCFE | #D35BFF 0xD2DF | #7A2BD9 0x795B | #E07BFF 0xE3DF |
| SPEAKING | #EAFFF4 0xEFFE | #9FF5CF 0x9FB9 | #3FD9A0 0x3ED4 | #1F9A7A 0x1CCF | #6FF0BF 0x6F97 |
| ERROR | #FFD6D6 0xFEBA | #FF6B6B 0xFB4D | #C7304A 0xC189 | #6B1A3A 0x68C7 | #FF5C5C 0xFAEB |
| OFF | #D8D4FF 0xDEBF | #8F86D9 0x8C3B | #5A4FB0 0x5A76 | #2E2870 0x294E | #7C72D0 0x7B9A |

State -> element mapping (both framebuffer and LVGL):

- State word: accent color, Headline size, +2px letter tracking, caps.
- Ring indicator (progress/spinner): accent. Track: `ringTrack`.
- Level meter lit segments: accent. Off segments: `faint`.
- Avatar glow: ramp f3 (outer) -> f0 (inner), alpha-weighted.
- Caption: ALWAYS `caption` #D8D2FF — never the accent. (Companion rule.)
- Thinking indicator: 60-degree arc at 300 deg/s on the ring + the accent.
- Listening: ring fills 0->360 deg over the 15s PTT cap, from the top.
- Speaking: ring EMPTY (companion rule — the ring only fills while
  recording); meter dances instead.
- Error: state word + dot in `danger`; glow ramp switches to error scheme.

## 4. Gradient + gloss recipes (cheap-primitive versions)

The Flutter originals use a 3-stop diagonal (top-left -> bottom-right)
gradient + a gloss overlay + a real blurred shadow. On a 410x502 framebuffer
at 25 FPS, all three are faked with bands and pre-rendering:

- **G1 — Surface gradient.** Vertical 3-stop: rows 0-50% lerp(top->mid),
  50-100% lerp(mid->bot). Diagonal is not worth its cost; vertical reads
  identically at watch size. Implementation: precompute a per-row color LUT
  once per (height, palette) — max 502 entries — then fill per row band.
  Band rows in groups of 4px with the same color: invisible on AMOLED,
  4x fewer fills. NEVER compute lerp per pixel.
- **G2 — Gloss band.** After G1: for the top 10px, inset x by 12px each side,
  blend white at alpha a(y) = 56 * (1 - y/10) over the surface color
  (`blend565`). Rounded top corners come free if drawn inside the round-rect
  clip. This is the single highest-value recipe in the system — it is what
  makes a rect read as a "bubble".
- **G3 — Inner highlight.** 1px line along the top inner edge (between the
  corners): white blended at alpha 56. Perimeter border at the alphas in
  section 1, drawn as a round-rect stroke using a color pre-blended against
  the known background (a stroke cannot carry alpha on 565 without blending
  per pixel; pre-blend against page bg or the gradient edge stop).
- **G4 — Fake shadow.** Shadows are rendered **at build time on the Mac**,
  never at runtime: render each bubble shape's shadow (offset (0,+8),
  gaussian blur 18) once into a 4-bit alpha sprite and ship the sprites
  as assets (3 shapes cover the OS: launcher tile 112x112, card 376xH,
  pill 88x40 — ≤24KB total, counted in the section 11 envelope). Runtime
  cost = one alpha blit of a small sprite, blended per-pixel only over
  the shadow's bounding box. There is NO runtime blur and NO scratch
  buffer. LVGL: use native shadow (width 18, ofs_y 8, color `blue`,
  opa 72) — LVGL renders it from its own partial draw buffers, no
  full-frame copy.
- **G5 — Page backdrop.** The backdrop is a vertical gradient, so it is
  drawn with the G1 scanline LUT directly into the framebuffer each
  frame: `ink` -> `night` -> `blueDeep` at 12% strength in the bottom
  third (a whisper of the companion's glossy backdrop photo, which does
  NOT ship to the watch). Zero buffers — pre-rendering it into a back
  buffer is banned by rule 0. Cost: 502 row fills of solid color, which
  the band flush has to move anyway.
- **G6 — Avatar glow.** 8 concentric discs drawn directly into the
  framebuffer, radius from stage_r+24 down to stage_r, colors from an
  8-entry LUT lerp(f3 -> black) recomputed only on state change. 8
  quantized steps read as a smooth glow on AMOLED at 40cm. No sprite,
  no buffer. (LVGL: two stacked circles with opa 85/40 in f2/f3, plus
  the blue 0x55-alpha glow circle behind, exactly like the companion's
  CustomPaint.)
- **G7 — Bezel gloss arc.** On the muse face ring: white at alpha 0.60,
  2px stroke, arc from 207 deg sweeping 90 deg (companion: start pi*1.15,
  sweep pi*0.5), round caps, at radius = ring outer radius - 1. One arc,
  huge perceived-quality payoff — it is chrome, present in every state.

## 5. Geometry

Corner-radius scale (use ONLY these):

| Token | px | Used by |
|---|---|---|
| r-chip | 10 | tags, keyboard keys, small-screen menu rows |
| r-card | 16 | overlay cards, inputs (18 acceptable), nested cards |
| r-bubble | 22 | THE radius: bubbles, launcher tiles, settings rows |
| r-dialog | 24 | pairing panel, modals, card overlay |
| r-pill | h/2 | status pill, PTT disc, buttons, battery |

Layout grid (410x502):

- Page margin: **20px** sides (content width 370). Status zone: **44px**
  top (up from today's 36 — the bigger type needs it). Bottom hint zone: 44px.
- Spacing scale: 4 / 8 / 12 / 16 / 20 / 24 / 32. Nothing else.
- Touch targets: min **56px** tall rows; **48px** absolute floor.
- Launcher: tiles **96x96**, r-bubble, margin-x 29, gap-x 28
  (29+96+28+96+28+96+29 = 402, centered), glyph 44px centered at tile
  top+16, label (Label size) 8px under the glyph inside the tile; row
  pitch 148. Page dots: 6px circles, 8px apart, accent when active,
  `faint` when not, 16px above the bottom edge.
- Muse face: stage disc diameter **300** centered at (205, 236); ring outer
  diameter **402** (screen width - 8), stroke **6**; state word at y=52
  (below status zone); caption bubble (r-bubble, pad 20x12, max width 330)
  centered, top edge 16px below the stage; meter (section 8) centered 20px
  below the caption.
- Settings rows: height **64**, r-bubble, 12px vertical gap, pad-x 20,
  icon 24px in a 40px r-pill chip (blue at 18% alpha over bubble), label
  left, value right in `dim`, chevron 16px in `faint`.
- Cards (overlay): **360 wide**, r-dialog, pad 20; title at Title size
  centered, body at Body size centered; TTL dismiss unchanged.
- Pairing panel: r-dialog, pad 24; code at Display-Pair size (section 6)
  in `blue`... no — code in accent of the CURRENT state (boot scheme),
  letter tracking +4. (Companion pairing cards are MuseBubbles; the code
  itself is the hero.)

## 6. Typography — the font plan

The 5x7 dot matrix is retired from all UI text (kept only as a debug/boot
fallback). It cannot read as premium at any size: at scale 2 its glyphs are
10x14 blocks; at scale 4 (today's weather hero) they are 20x28 blocks with
4px pixel steps. Apple Watch text is anti-aliased — that single difference
is most of the perceived quality gap.

- **Typeface: Inter** (SIL OFL 1.1 — embeddable, no fee, SF-adjacent
  metrics, drawn for small UI text). Weights: Regular, SemiBold, Bold.
  Acceptable substitute: Roboto (Apache 2.0). For LVGL (muse-gadget-206):
  **Montserrat** (OFL) — already the firmware's secondary font, so the
  gadget stays in-family; bake it at the same sizes with 4bpp AA.
- **Type scale (px on 410x502):**

  | Token | Size / weight | Line height | Use |
  |---|---|---|---|
  | Display | 96 Bold, tabular | 1.0 | digital clock (HH:MM) |
  | Display-Pair | 40 Bold, tracking +4 | 1.1 | pairing code |
  | Title | 28 SemiBold | 1.15 | screen titles, card titles |
  | Headline | 20 SemiBold, caps, tracking +2 | 1.2 | state word, section heads |
  | Body | 18 Regular | 1.25 (22px) | replies, notification body, settings labels |
  | Caption | 16 Regular | 1.25 | avatar caption, secondary body |
  | Label | 14 SemiBold | 1.2 | launcher labels, values, status pill |
  | Micro | 15 SemiBold, tabular | 1.0 | status-bar clock + battery % |

- **AA approach:** 4-bit coverage (16 levels) baked at build time. Atlas =
  8-bit coverage map generated by FreeType, quantized to 4-bit and packed
  2px/byte. Renderer = `blend565(dst, fg, coverage)` read-modify-write —
  text covers a small fraction of the frame, so per-pixel blending is
  affordable HERE (and only here; fills use G1 banding).
- **Generator (committed tool):** `tools/gen_font.py` — Python + Pillow
  (Pillow 11.3.0 is already installed on the Mac; `PIL.ImageFont` drives
  FreeType and returns exact coverage bitmaps). Emits a C header (or a .bin
  for the assets FAT partition) per size/weight with glyph bitmaps +
  metrics (advance, x/y bearing, line height). No kerning (watch UI does
  not need it); tabular figures ON for time/battery.
- **Charset:** ASCII 32-126 **plus** `° … – — ' ' " " • ← → ↑ ↓ ⚡` and the
  icon glyphs of section 7 in the Private Use Area. This closes today's
  holes: the current 5x7 has NO `" # $ & ( ) ; = @ °` — unsupported chars
  render as hollow boxes, and weather prints `22C` for want of `°`.
- **Asset budget (hard cap, rule 0): ALL font + icon + shadow assets in
  one firmware image ≤ 256KB of flash.** Sized plan (4-bit coverage,
  ~110 glyphs/size incl. PUA icons, estimates from cell area x 0.5B/px):

  | Asset | Subset | Est. |
  |---|---|---|
  | Inter 15 SemiBold (Micro) | full charset + icons | ~9KB |
  | Inter 16 Regular (Caption) | full charset + icons | ~10KB |
  | Inter 18 Regular + SemiBold (Body) | full charset + icons | ~24KB |
  | Inter 20 SemiBold (Headline) | full charset + icons | ~14KB |
  | Inter 28 SemiBold (Title) | full charset + icons | ~27KB |
  | Inter 40 Bold (pairing) | **subset: 0-9 A-Z only (36 glyphs)** | ~17KB |
  | Inter 96 Bold (clock) | **subset: 0-9 : . A P M ° (16 glyphs)** | ~45KB |
  | Shadow sprites (G4, 3 shapes) | — | ~24KB |
  | **Total** | | **~170KB of the 256KB cap** |

  The big sizes survive ONLY by subsetting — a full-charset 96px atlas
  would be ~300KB alone. If the generator's real output exceeds the cap,
  cut order: 40px pairing font (fall back to 28), then 15px (derive
  Micro from 16), never the 18/20/28 core. Assets live in the existing
  ~5MB `aiwatchos_assets` FAT partition (the 2MB app slot is not
  touched); they may alternatively be flash-mapped — either way the
  RENDERER reads them in place and any working cache is PSRAM (≤64KB,
  section 11). The generator prints the byte total and FAILS the build
  over 256KB.
- **Hermes dialect:** same pipeline, sizes 16/20/28 + a 40px wordmark
  subset (HERMES + gadget letters only) ≈ 65KB — same cap, large margin.
- **LVGL:** regenerate Montserrat 16/20/28 via `lv_font_conv` at 4bpp,
  full charset above (≈120KB measured class for LVGL's format — inside
  the same 256KB cap for the gadget's own image); DELETE unscii from
  all chrome (state word, caption, power label). Unscii is a pixel
  font — on the gadget it is currently the voice of the state word and
  the captions, the two most-read strings.

## 7. Iconography

- **Style:** 2px-stroke rounded line glyphs (Lucide/Feather geometry —
  MIT/ISC licensed), drawn as 4-bit alpha masks from the same generator
  pipeline as the font (icons are PUA glyphs in the atlas). One stroke
  weight, round caps/joins, no filled icons except state dots and the
  battery fill.
- **Required set (30):** clock, sparkle (Muse), feather (Hermes), gear,
  bell, cloud-sun, cloud, cloud-rain, cloud-snow, sun, moon, thermometer,
  droplet, wind, mic, mic-off, speaker, wifi, wifi-off, bluetooth,
  battery, battery-charging, check, x, chevron-right, chevron-down,
  arrow-left, heart, play, pause.
- **Launcher tiles:** bubble gradient (G1+G2) in the blue family; per-app
  tint of the gradient FOOT only: clock `bubble` (neutral), muse `bubble`,
  settings slate #2A3B55 (0x29CA), notifications `bubble`, weather sky
  #1B4F8A (0x1A71). **One exception: the Hermes tile is gold-tinted**
  (Hermes dialect bubble, section 9) — it teaches the two-brand system at
  a glance. Glyph in `text`, label in `text` at Label size.
- **Status icons:** wifi/bluetooth/battery as glyphs at 16px in `dim`;
  connected state = `text`; the battery is drawn geometry (section 8),
  not a glyph.

## 8. Components (recipes with numbers)

- **Status bar (all screens, drawn by the OS shell ONLY — apps never paint
  y < 44):** transparent over the G5 backdrop (NO filled band — the filled
  band is what makes today's bar look like a 2009 phone). Left: time at
  Micro size in `text`. Right: link glyph + battery: outline 25x12,
  r=3, 1px stroke `dim`, nub 2x6, fill = level width in `text`
  (charging = `ok`, <20% = `danger`), % text at Micro in `dim` left of
  the outline. Center: nothing (or the link dot, Hermes-style, on the
  clock face only).
- **PTT disc (muse face, replaces "hold the bottom strip"):** 76px circle
  at bottom center, filled-bubble gradient (blueTop->blue->blueBot),
  mic glyph 30px in `mist`, gloss band G2 across its top; while listening
  it scales to 84px and the ring takes over. Press feedback: +12%
  brightness overlay, 120ms.
- **Level meter (canonical, both OSes):** 14 segments, 9x9, r=3, gap 4,
  centered; lit = state accent with per-segment history (a falling-peak
  trace, NOT one uniform height — today's AIWatchOS meter moves all 24
  bars together as one block, which reads as a progress bar, not audio).
  Ballistics: fast attack, release lerp 0.2/frame at 25 FPS (already the
  AIWatchOS mic RMS ballistics — keep the numbers, fix the rendering).
- **Bubble card (chat/notification):** G1 gradient (`bubble` family for
  others, filled-blue family for own/primary), r-bubble, border alpha per
  section 1, G2 gloss, pad 16x12; meta line (time/status) at Label size
  in `dim` (on blue: `mist` at 70%).
- **List row:** section 5 settings row; pressed = gradient foot color
  (`bubbleBot`) + 120ms; selected (radio-style) = 1px `blue` border.
- **Notification card:** app glyph chip 40px r-pill left, title Body
  SemiBold, body Caption in `dim`, time Label in `faint` right-aligned;
  cards stacked with 12px gaps — a LIST, newest first (today only the
  latest exists visually and its body renders one word per line — see
  audit).
- **Pairing panel:** r-dialog bubble, title Title, code Display-Pair in
  boot accent #A9C0FF, hint Caption in `dim`, spinner = ring arc.
- **Battery (device screens):** as status bar but 200x20 track: r-pill
  outline `faint`, fill state-colored, % at Body size right of it.

## 9. Hermes variant tokens (HermesGadget only)

Same geometry, radii, type scale, gloss recipe. Palette swap ONLY:

| Muse token | Hermes token | Hex | RGB565 |
|---|---|---|---|
| blue (primary) | gold | #F2B33D | 0xF587 |
| blueDeep | goldDim | #6E521E | 0x6A83 |
| bubble | bar | #181F2A | 0x18E5 |
| bubbleTop (computed) | — | #3D434C | 0x3A09 |
| bubbleBot (computed) | — | #2E2C27 | 0x2964 |
| ink/night (bg) | bg | #0A0E14 | 0x0862 |
| text | text | #E8EEF2 | 0xEF7E |
| dim | dim | #8492A0 | 0x8494 |
| faint | faint | #343E4C | 0x31E9 |
| shadow color | gold at alpha 0.28 | — | — |

Hermes state colors are already semantic and stay: listening green
#3DD68C (0x3EB1), thinking blue #5AA9F2 (0x5D5E), responding/speaking
gold #F2B33D, error red #F25F5C (0xF2EB), warn yellow #F2C94C (0xF649),
link dot: offline red / net yellow / online green #3DD68C.
Prompt buttons: YES = green-dim fill #1C5C3E (0x1AE7) with gloss added,
NO = red-dim #7A2C2C (0x7965) with gloss added, both r-bubble, text `text`.
The 1-bit mascot is ART, not chrome — it stays 1-bit white (see audit for
why), optionally tinted `text`; its caption takes the state color as
today (that rule is Hermes-local and works).

## 10. Motion

Frame budget: 25 FPS / 40ms everywhere (AIWatchOS loop and LVGL tick
already agree). All durations in ms, all easing ease-out cubic unless noted.

- **App switch: brightness-dip, 240ms total.** Ramp the CO5300
  brightness register to 0 over 120ms, switch content at black, ramp
  back over 120ms. Zero extra buffers (rule 0) and it reads as a
  deliberate fade, not a cut. (Framebuffer crossfades would need a
  second 412KB frame buffer — banned. LVGL gadget: tileview slides and
  `lv_screen_load_anim` FADE_ON 200ms using LVGL's own partial draw
  buffers only.) NO hard cuts at full brightness anywhere.
- **State change (muse):** palette cross-fade expEase rate 7; pose/motion
  blend rate 14 for gaze (companion numbers). Ring spinner 60 deg at
  300 deg/s; listen arc linear over the 15s cap; speaking meter live.
- **Avatar life:** blink every ~4.5s (2-frame shut at 25 FPS = 80ms);
  breathing bob = sin(1.5 Hz... companion: aura radius sin(seconds*1.5),
  amplitude 4 cells) — keep gadget's existing avatar motion numbers,
  they are already companion-matched.
- **Press feedback:** 120ms, scale 0.96 on tiles / brightness +12% on
  rows and discs.
- **Card overlay in:** 180ms fade + rise 8px. Out: 140ms fade.
- **Sleep/wake:** backlight ramp 200ms (CO5300 brightness steps), then
  SLPIN; wake = reverse. (Power work is another workstream; the ramp is
  the UI's part.)
- **Scroll:** notifications/settings lists scroll with 8-frame (320ms)
  momentum decay; scroll thumb min 24px long, 4px wide, r-pill, `dim`,
  track `faint` at 40%.

## 11. Resource budget (hard limits — rule 0 expanded)

Platform: ESP32-S3, **512KB SRAM (the scarce resource)**, 8MB PSRAM,
32MB flash, app slots 2MB (post power-pass layout), integrated firmware
already ~1.14MB before UI assets → ~860KB free in the app slot, and the
assets FAT partition has ~5MB. The design spends flash freely-ish and
SRAM almost not at all:

| Resource | Budget | Where it goes | Why it's safe |
|---|---|---|---|
| Font+icon+shadow assets | **≤ 256KB flash per firmware image** (plan: ~170KB, section 6) | assets FAT partition (or flash-mapped) | Leaves ≥85KB headroom under the cap and never touches the 2MB app slot; generator fails the build over the cap |
| Glyph/text cache | **≤ 64KB, PSRAM only** | optional pre-blended sprites for static strings (clock date line, hints) | PSRAM is 8MB and already hosts the framebuffer (412KB) + turn buffers; 64KB is <1% |
| SRAM for UI | **≤ 4KB total** | gradient LUT (502 rows x 2B = 1,004B), glow LUT (8 x 2B), per-line staging (410 x 2B x 2 lines = 1.6KB) | 512KB SRAM is shared with WiFi/BLE/audio stacks; 4KB is noise. NO full-screen buffer in SRAM at any size, at any time |
| Frame buffers | **exactly 1** (410x502x2B = 411,640B, PSRAM) + the existing DMA bounce buffer (20 rows) | already allocated by the OS today | Animation/transitions are brightness-dip + in-place band redraws (sections 4, 10) — nothing in this system allocates a second frame |
| Per-frame CPU for gloss | gloss band ≤ 370x10px blends; gradient = LUT row fills; shadow = 3 small sprite blits | — | Blending is confined to text pixels, gloss bands, and shadow boxes — together <5% of the frame's pixels; fills stay solid-color row writes |
| QSPI flush | band/dirty only | HAL already supports y0..y1 ranges | Full-frame flush ≈ 412KB ≈ ~80ms at 40MHz-class QSPI — over the 40ms frame budget; band flushing is a correctness issue, not an optimization (audit X7) |

Numbers chosen so the UI can be added to the integrated firmware
WITHOUT renegotiating anyone else's memory: the transport, audio, and
power workstreams keep their budgets; the UI's entire SRAM ask is 4KB
and its entire flash ask fits in the assets partition with the app
slot untouched.
