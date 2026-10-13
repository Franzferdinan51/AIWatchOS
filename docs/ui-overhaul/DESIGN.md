# AIWatchOS UI/UX Overhaul — Design Document

Status: DESIGN ONLY (research + spec). No firmware code in this pass.
Written 2026-10-13. Tree location note: the stage-2 hardware wave owns
~/aiwatchos-flash, so this draft lives at ~/ui-overhaul-DESIGN.md (Mac home)
instead of docs/ui-overhaul/DESIGN.md; move it into the tree when the tree is
free. It extends, and does not replace, docs/ui/DESIGN_TOKENS.md ("Muse Gloss
v1.0") and docs/ui/DESIGN.md in AIWatchOS.

Scope rule for everything below: every screen is specified ONLY against
primitives the native canvas already has (see §0.3), or against a renderer/OS
addition that is explicitly costed in §4. If a spec element is not in either
place, it is not in the design.

---

## 0. Ground truth the design is built on

### 0.1 Hardware / panel
- Waveshare ESP32-S3-Touch-AMOLED-2.06, CO5300 AMOLED, 410×502 px, touch
  (FT5x06-family), BOOT + PWR buttons, AXP2101 power, ESP32-S3: 512 KB SRAM
  (the scarce resource), 8 MB PSRAM, 32 MB flash, 2 MB app slots.
- One PSRAM framebuffer 410×502×2 B = 411,640 B. No second framebuffer.
  Band flush: 4 horizontal bands, FNV-hash dirty detection, ~80 ms full-frame
  flush vs a 40 ms frame budget → full-screen repaints are for state changes,
  cheap partial redraws are the norm.

### 0.2 Budgets (standing orders, unchanged)
- UI static SRAM ≤ 4 KB total. Buffers, fonts, sprites live in PSRAM/flash.
- Baked font+icon+shadow assets ≤ 256 KB flash total. Measured today:
  11 font files ≈ 110.7 KB + sprites.bin 50,328 B ≈ 161 KB → ~95 KB headroom.
- Baseline gate at design time: app image 1,306,512 B, static DIRAM
  117,323 B. Every stage re-gates on image size + DIRAM before "flash-ready".

### 0.3 What the renderer can do TODAY (read from source, 2026-10-13)
Canvas (components/aiwatchos_os): put_pixel/blend, fill, h/v lines, fill_rect,
fill_round_rect, stroke_round_rect (blended), circles, ring, arc (deg, 0=east,
clockwise), 3-stop vertical gradients (fill_vgrad, fill_round_rect_vgrad,
LUT-free), draw_bubble (the full G1+G2 gloss + G3 border recipe, optional
pre-baked alpha-4 shadow sprite), blit_alpha4 (+vstretch), greedy wrap_text,
legacy 5×7 text (boot/debug only by policy).
AA text: draw_text_aa (+centered/right), pixel-width measure, faces baked as
AIF1 in the FAT assets image, blitted straight from PSRAM (0 KB glyph cache):
Micro 15 SemiBold, Caption 16, Body 18, Body SemiBold 18, Headline 20
SemiBold, Title 28 SemiBold, Pair 40 Bold (digits+uppercase subset),
Display 96 Bold (subset: 0-9 : . A P M °), icon faces 20/40 (PUA glyphs),
weather icons 64. If assets are not ready, EVERYTHING falls back to the 5×7
dot matrix — which is the crude look on glass today; the font-load fix is a
hardware-wave prerequisite (§5 Stage 0), not part of this design.
Shell (main/app_main.cpp): G5 backdrop gradient → app → status zone LAST
(44 px: time left Micro, battery cluster right, 3 px Muse-link strip at
y41–43). App switch = brightness DIP, 6 frames {45,15,0,15,45,100} ≈ 240 ms
(there is NO slide in AIWatchOS; the slide is muse-gadget-206's LVGL
tileview — see §6). Bottom-30 px strip press → launcher. BOOT hold ≥3000 ms
→ launcher (escape). Sleep after 30 s (Kconfig), PWR short-press toggles
sleep, wake press is swallowed until fingers lift.

### 0.4 Defects in the CURRENT screens this overhaul fixes
1. Clock face: 24-hour only; huge dead zone y≈340–502; link state not shown
   (the clock app has no OS data path to net/link/weather state at all);
   two unexplained dots are the only "complications".
2. Hermes app: most text still renders through the LEGACY 5×7 draw_text at
   integer scales (title scale 3, body scale 2) even when Inter is loaded —
   it looks broken next to every AA screen. Layout is a top-down text dump:
   full-screen flat bg fill (kills the shell backdrop), a redundant 200 px
   battery bar duplicating the shell's battery cluster, conversation as
   flowing wrapped text instead of turns.
3. Settings: values are RAM-only (nothing persists); Do Not Disturb is a
   dead toggle (nothing consumes it); no time-format or watch-face setting
   exists to persist anyway.
4. Muse app: caption wraps at 300 px inside a 370 px content width; the
   "heard" line competes with the reply; no unpaired/offline state design
   (states exist in the link layer, not as designed screens).
5. Notifications: no empty state (blank screen when the store is empty),
   no scroll thumb while dragging, actionable cards carry no in-card
   buttons (actions live only in the Hermes app / OS prompt path).
6. Weather: honest but bare — staleness is a text line, no tap-to-refresh,
   one generic empty state for four different failure reasons.

---

## 1. Research findings, mapped to our constraints

All external sources were read 2026-10-13; full citations in §8. Findings are
paraphrased, not quoted.

### 1.1 What earns space on a watch face
- Watchy (200×200, 1-bit e-paper) community faces converge on the same five
  elements: time, date, battery, weather, steps — at a resolution where every
  element must justify itself. Crispface (a Watchy firmware with composable
  complications) adds one more transferable idea: each complication carries
  its own refresh cadence, and the watch only syncs as often as the fastest
  complication demands. [S6][S7]
- The LVGL smartwatch demo face (LVGL's own showcase) shows time, date,
  battery and one health metric, with peer screens reached by swipe (health,
  music, analog face, weather → forecast). [S4]
- AsteroidOS 2.0 added a LARGE charging status on selected faces while
  charging, and a nightstand mode whose whole job is showing charge state
  clearly — charging is a face-level state, not a settings detail. [S1]
- MAP: our face (§3.1) = time (Display 96) + date + three complication
  chips (weather / Muse link / Hermes link) + one alerts line. Steps/HR are
  NOT complications: AIWatchOS publishes no step or heart-rate service
  today, and the design does not draw data the OS cannot supply (§6).
  "Next event" is likewise cut — InfiniTime-ecosystem research notes
  nothing upstream ships a next-event complication without a phone-side
  data piece we do not have; the slot carries the alerts summary instead,
  which is real data (notification store count). [S3]

### 1.2 Navigation model
- AsteroidOS uses a spatial model: from the face, app drawer below, quick
  settings above, agenda right, notifications left; swipe down inside any
  app goes home, swipe right goes back. It is elegant and it is a gesture
  vocabulary our shell deliberately does NOT have (§6). [S2]
- The PineTime UI proposal (arteeh) uses the same centre-out map (face in
  the middle, favourites, app grid, notifications around it) and one detail
  worth stealing: inside the app grid and notification list, a slim top bar
  with time + battery appears — the ancestor of our shell status zone,
  which we already render on every screen. [S3]
- Wear OS app quality rules: swipe-to-dismiss from almost all screens,
  preserve app state across backgrounding, show a scrollbar while a list is
  being scrolled. [S5]
- MAP: keep our model (one big bottom-strip target → launcher; BOOT-hold
  escape; PWR sleep). It is coarser than AsteroidOS's and that is a feature
  on a 410 px touch panel with two AI apps: there are exactly 7 destinations.
  Borrowed: scrollbar-while-scrolling for Notifications (§3.6) and the
  state-preservation rule (already true via pause/resume).

### 1.3 Launcher model
- InfiniTime: app list/grid behind one swipe, plus a quick-actions surface
  (brightness, do-not-disturb, flashlight) that is NOT the full settings
  app. AsteroidOS 2.0 went further: its QuickPanel replaces QuickSettings
  and mixes toggles WITH app shortcuts, and the launcher itself ships in
  multiple selectable styles. [S1][S9]
- AmoledOS (ESP32-S3 + AMOLED sibling project, LVGL): settings are a flat
  list — brightness, always-on, language, Wi-Fi, Bluetooth, watchface,
  touch calibration — and its flashlight is simply the panel driven full
  white, the only honest flashlight on AMOLED. [S10]
- MAP: grid, not list, not honeycomb (§6): 6–7 apps fit one screen at
  112 px tiles — bigger than any reference's targets. Borrow the
  QuickPanel idea as a 3-pill quick strip at the launcher's bottom
  (DND / Light / brightness step) instead of a new surface (§3.2).

### 1.4 Touch targets
- Wear OS app quality WO-V2: minimum 48×48 dp touch targets. Apple HIG:
  44×44 pt minimum, 60+ pt for primary actions. On our ~315 ppi panel the
  literal dp conversion is not the binding constraint; the in-tree token
  floor (rows ≥56 px, 48 px absolute floor) plus on-device verification
  is. Every target in §3 is ≥56 px except in-card mini-buttons at 48 px,
  which sit inside a ≥64 px card row context. [S5]
- AmoledOS's calculator is explicitly "sized for a thumb rather than for
  density" — the right instinct, adopted as a review rule: if a target
  needs a fingernail, it fails review. [S10]

### 1.5 Typography
- AsteroidOS 2.0's weather overhaul was specifically a legibility pass
  (cleaner font, better handling of long place names) — evidence that on
  watches, type IS the UI. [S1]
- muse-gadget-206 (our sibling, SAME panel) ships Montserrat Medium
  12/14/16/20/28. Our baked Inter ladder (15/16/18/20/28/40/96) is coarser
  at the small end — nothing below 15 px anywhere — which is the right
  side of the legibility line; §3 enforces "no text below Micro 15" as a
  hard rule, including in the Hermes app, whose legacy scale-2 text
  (~14 px dot matrix) is the worst offender.
- Rule adopted from all references: ONE typeface per dialect surface
  (Inter everywhere; Hermes differs by colour and surface, not by font).

### 1.6 Status bar conventions
- Every reference keeps battery + time visible inside lists/menus
  (arteeh proposal; InfiniTime quick settings show battery prominently).
  Our shell already does this better than most: status zone on EVERY
  screen, app content starts below it. Kept unchanged. One change only:
  the clock face stops duplicating battery as mystery dots (§3.1). [S3]

### 1.7 Empty / loading / error states
- Pattern across references (InfiniTime settings/faces, LVGL demo power
  states, AmoledOS): an empty state is an icon + one-line headline + one
  line of "what happens next", never a blank screen and never a spinner
  without words. The Infineon LVGL smartwatch example formalises power
  states: full UI → simplified always-on UI after 30 s idle → display off —
  the same ladder as our wake → dim → sleep, and our dim face (§3.1)
  follows its "simplified, time-first" shape. [S4]
- MAP: §3 gives EVERY screen explicit empty/loading/error/dim states in a
  shared recipe: icon face glyph (40 px) + Title headline + Caption
  explanation + (only when an action exists) one ≥56 px pill.

### 1.8 Motion
- AsteroidOS: smooth transitions + inertial scrolling, deliberately
  minimal decoration. Our motion tokens already encode this (25 fps,
  40 ms frames, press feedback 120 ms, momentum 8 frames). The one gap
  vs references is app switching: LVGL siblings slide (tileview);
  we dip (≈240 ms brightness dip). The dip is honest, cheap, and reads
  as a blink-cut; a slide is costed as an OPTIONAL renderer addition
  (§4 R5) and is the lowest priority in the build order (§5 Stage 6).

---

## 2. Token extensions — "Muse Gloss v1.1" (additions to DESIGN_TOKENS.md)

v1.0 tokens (colours, G1–G7 recipes, geometry, type scale, motion) are
unchanged. This overhaul adds only the following:

### 2.1 Time format
- `time.format` = 12H (default) | 24H. A user setting (§3.3), persisted.
  Everywhere time is drawn — clock face, status zone, notification stamps,
  Hermes stamps — the format applies. Status-zone Micro time switches too
  (it is the most-seen clock on the watch).
- Display 96 subset already contains A, P, M — 12-hour needs NO new glyphs.

### 2.2 New shared components (token-level)
- `chip`: r10, fill `input` #13233C, h 56, padding 12. Contents: icon20 +
  Caption text. Used for clock complications and Hermes user bubbles are
  NOT chips (see chip-hermes below).
- `pill-status`: r = h/2, h 28, fill `bubble` (blue) / `bar` (gold dialect),
  8 px state dot + Caption word. The dot is a state element; state colours
  are legal here per the v1.0 accent rule.
- `card`: existing bubble recipe, r18 lists / r16 content (unchanged).
- `ptt-pill`: 370×64, r32. Blue dialect: bubble fill + mic icon40. Gold
  dialect: bar fill + 2 px gold stroke; recording state = solid gold fill,
  dark label. (Muse keeps its 72 px PTT disc — §3.4 — because its stage
  composition is circular; Hermes is a list and gets the pill.)
- `quick-pill`: 112×56, r28, input fill, icon20 + Micro label. Launcher
  quick strip only.
- `dialog`: r24 card, w 330, centred; scrim = backdrop dimmed via the new
  `fill_rect_blend` (§4 R2) at alpha ~55% black.
- `empty-state` recipe: icon40 in `faint` at centre y≈190, Title headline
  y≈250, Caption explanation y≈286 (≤2 lines, wrap 300), optional pill
  y≈340. One recipe for every screen (§1.7).

### 2.3 Hermes dialect additions (hcolors family only)
- `slate bubble` (user turns): top #3D434C / mid #272E36 / bot #1B2129 —
  one step lifted from the existing bar ramp, so user turns read as
  "same room, different speaker" without leaving the gold dialect.
- Hermes text moves entirely to the shared Inter ladder (§3.5). The gold
  dialect remains colour + surface only.

### 2.4 Dim ladder (sleep states)
- Reuse the switch-dip brightness table as named levels: `bright.full`
  100, `bright.dim` 45, `bright.face-dim` 15, `bright.off` 0. The clock
  dim state (§3.1) draws at face-dim with dim-token colours; no new
  rendering path.

### 2.5 Hard rules (review checklist)
- No text below Micro 15. No legacy 5×7 text in any app screen.
- Targets ≥56 px (48 px floor only for in-card mini-buttons).
- State colour appears ONLY in: state word, ring/arc, level meter,
  avatar glow, 8 px dots, pill-status dot, LIVE/STALE pill. Chrome is
  always dialect colour (blue / gold).
- Content width 370, margins 20, spacing scale 4/8/12/16/20/24/32 — v1.0.

---

## 3. Screen-by-screen specs

Wireframes: 1 char ≈ 10 px wide, 1 row ≈ 20 px tall (41×25 grid ≈
410×502). Px annotations are authoritative; the ASCII is proportion guide.
"SZ" = shell status zone (44 px, drawn by the shell on every screen).

### 3.1 Clock face (app "clock") — 12-hour, complications

Zones (digital face):
- y0–44 SZ (shell): time Micro left, battery right, link strip y41–43.
- y56–84 DATE: Caption 16, `dim`, centred, uppercase ("TUE, OCT 13").
- y96–208 TIME: Display 96, `textNum`, centred: hour (no leading zero) +
  ":" + minutes. AM/PM: Title 28 SemiBold, `dim`, baseline-aligned 8 px
  right of the minutes. (If a future locale made this exceed 370 px, the
  fallback is AM/PM as Micro centred under the time — measure at build
  with text_width_aa; expected width ≈300 px, fits.)
- y232–256 NEXT LINE: centred Caption `dim`: the single most useful
  sentence the OS can prove — priority: (1) charging: "CHARGING · 72%"
  in `ok`; (2) unread alerts: "2 NEW ALERTS"; (3) weather age when
  stale: "WEATHER STALE · 1H AGO" in `warn`; (4) blank.
- y288–344 COMPLICATION CHIPS (h 56, r10 `chip`): three chips —
  [weather: wx icon20 tinted + "72° CLEAR" Caption] flex width,
  [Muse: 8 px link dot + "READY" Caption] w 108,
  [Hermes: feather icon20 gold + "LINKED" Caption] w 108.
  Chip text is the SHORT state word only (Ready / Thinking / Offline /
  Linked / Pairing / Off). Tap a chip → opens that app.
- y372–440 BREATHING ROOM: deliberately empty. The face has one job
  — be read in a single glance; the space is absorbed by generous
  zone sizing above, not by inventing content.
- y458–502 HINT ZONE: Micro `faint` centred: "TAP BELOW FOR APPS".

```
+-----------------------------------------+  y0   SZ (shell)
| 3:27 PM                  [####] 100%    |
+ - - - - - - - - - - - - - - - - - - - - +  y41  link strip
|                                         |
|              TUE, OCT 13                |  y56  date
|                                         |
|           3 : 2 7   PM                  |  y96  Display 96
|                                         |
|              2 NEW ALERTS               |  y232 next line
|                                         |
| [~ 72° CLEAR] [• READY] [f LINKED]      |  y288 chips h56
|                                         |
|                                         |
|           TAP BELOW FOR APPS            |  y458 hint
+-----------------------------------------+  y502
```

Data path (REQUIRED, §4 R4): the clock app reads a shell-provided
StatusSnapshot { wifi state, muse link state, hermes link state,
weather snapshot ref, unread count, power }. Today the clock has no
such path; without R4 this screen cannot exist. Stamps/ages are
computed from data the services already keep.

States:
- Normal: as above.
- Weather stale: wx chip keeps last temp, word becomes "72° · STALE",
  word in `warn`. No data yet: chip shows wx icon + "NO WEATHER".
- Offline (Wi-Fi down): wx chip = wifi-off icon + "OFFLINE"; Muse and
  Hermes chips show their real link words (they will read OFFLINE too —
  that is information, not noise).
- Charging: NEXT LINE shows CHARGING (priority 1); SZ battery fill `ok`
  (shell already does this).
- Sleep-dim (panel at bright.face-dim, after idle on the face): ONLY
  date (Micro) + time (Display, colour blended 60% toward ink) draw;
  chips, next line, hint hidden. Any touch/button wakes to full face.
- Analog face (selectable, §3.3): existing dial unchanged (ringTrack
  bezel, weighted hands, blue second hand) + date Caption under centre;
  NO chips (the dial is the composition). Same dim state minus hands'
  second hand.

### 3.2 Launcher (app "launcher")

- Grid 3 cols × 2 rows, tiles 112×112 r24 bubble + shadow sprite,
  x0 = 25, pitch 128; rows y = 72 and y = 216 (gap 32). Glyph icon40
  centred at tile+16..56; label Caption 16 centred at tile+72.
  ("Notifications" fits a 112 px tile at Caption — verify by measure;
  fallback label "Alerts".)
- Order: Muse (mic) / Hermes (feather, gold tile + gold foot bar) /
  Weather (cloud-sun) / Alerts (bell) / Settings (gear) / Clock (clock).
  The old "Apps" tile is REMOVED — the launcher IS apps; a tile that
  opens the screen you are on is noise.
- Alerts badge: 20 px circle `danger` at tile top-right, count in Micro
  `textNum`, shown only when unread > 0 (count from the store via R4).
- Quick strip y = 368–424: three quick-pills (112×56):
  DND (moon glyph; filled blue + "ON" when active), Brightness step
  (sun glyph; cycles 25/50/75/100, label shows the current %),
  Light (sparkle glyph; opens the Light screen).
- Light screen (new mini-app, trivial): full-screen `mist` fill,
  brightness forced 100, Micro `ink`-dark "TAP TO EXIT" bottom; any
  press exits. AMOLED-honest flashlight per §1.3.
- y458–502 hint zone: BOOT-hold leads TO the launcher, so there is no
  escape hint to show here, and the bottom-strip fallback targets the
  launcher itself. The zone instead carries a Micro faint wordmark,
  "AIWATCHOS" — the home screen is the one place a brand line earns
  its pixels. Returning to the face = the Clock tile.
- Growth rule: a 7th app paginates (page = 6 tiles + quick strip stays
  fixed; swipe left/right between pages, page dots Micro in hint zone).
  Do NOT shrink tiles to fit more.

```
+-----------------------------------------+  y0   SZ
| 3:27 PM                  [####] 100%    |
+ - - - - - - - - - - - - - - - - - - - - +
|                                         |
|  [Muse ]  [Hermes]  [Weathr]            |  y72  tiles 112²
|                                         |
|  [Alerts•2] [Setngs]  [Clock ]          |  y216
|                                         |
|  ( DND )  ( BRIGHT 75% )  ( LIGHT )     |  y368 quick strip
|                                         |
|              AIWATCHOS                  |  y458
+-----------------------------------------+  y502
```

States: press feedback = white wash ~120 ms (existing). DND on: DND
pill filled + launcher gains NO other change (DND must be checkable at
a glance — the pill is the indicator; the clock face shows a moon
Micro in the NEXT LINE when DND is on and nothing outranks it except
CHARGING… priority: CHARGING > DND > alerts > stale).

### 3.3 Settings (app "settings")

Geometry kept (rows h 64, pitch 76, y0 56, x 20 w 370 r18 bubble, 40 px
icon chip) — it already meets the tokens. The overhaul is content +
persistence + honesty:

Rows (main page, scrolls if needed):
1. Brightness — sun chip, slider (existing mechanics: track x190–330,
   14 px thumb, % Micro readout at row right edge).
2. Volume — volume chip, slider.
3. Do Not Disturb — moon chip, 44×24 switch (hit area = whole row).
   NOW REAL: DND suppresses notification panel-wake and banners;
   Hermes approval prompts still wake (they are time-boxed; silently
   expiring an approval is worse than a wake — FLAGGED DECISION, §7).
4. Time Format — clock chip, value right "12H" (tap cycles 12H/24H).
5. Watch Face — sparkle chip, value right "DIGITAL" (DIGITAL/ANALOG).
6. Sleep After — value row (no chip), value right "30S" (30S/60S/2M;
   writes the existing Kconfig-backed timeout seam).
7. Speak Replies — mic chip, switch (the one registered external
   toggle; unchanged seam).
8. About — info chip, chevron; right value "BATTERY 72%" stays.

Persistence (§4 R6): every row writes NVS on change and loads at boot;
defaults = today's behaviour, except Time Format defaults 12H per the
overhaul order. No other screen may keep user settings in RAM only.

About page (sub-page, chevron rows → back via bottom strip / BOOT is
NOT back; back = a "‹ BACK" row at top, h 56 — depth stays ≤2):
compact label/value rows, values presence-only (privacy rule):
Wi-Fi (SSID never shown — "CONNECTED"/"OFF"), Muse Token
(PRESENT/MISSING), VM, Muse Link, Hermes Pairing, Speech (NO KEY/ON/
OFF), Fonts ("11/11" from assets_load counts — the number whose absence
hid the empty-assets bug), Firmware (app version string — pairs with
the MDB 1.1 `mdb version` gap), Battery.
States: unknown value = "—" (Micro dim); a DOWN link is `warn`, never
`danger` (away-from-home is a normal state, not a fault).

```
+-----------------------------------------+  y0   SZ
| 3:27 PM                  [####] 100%    |
+ - - - - - - - - - - - - - - - - - - - - +
| SETTINGS                                |  y52  Headline caps
| (o) Brightness      [====----]  60%     |  y76  rows h64
| (o) Volume          [======--]  75%     |
| (o) Do Not Disturb            [ off ]   |
| (o) Time Format                 12H ›   |
| (o) Watch Face              DIGITAL ›   |
| (o) About                 BATTERY 72% › |
+-----------------------------------------+
```

### 3.4 Muse app (app "muse") — polish, not redesign

The ring/stage/avatar composition is the strongest screen in the OS and
matches the sibling gadget's proven layout for this panel. Keep:
bezel ring R188 (centre 205,232), stage disc R138, avatar 256 px,
state word Headline accent y50, agent-status Caption y76, level meter
inside stage at cy+96 (14 × 9 px, gap 4), LISTENING arc / THINKING
60° spinner at 300°/s, SPEAKING = ring empty, PTT disc 72 px y424–496,
press y > H−100 starts PTT.

Changes:
1. Caption bubble: width 370 (x20), text wraps at 330 (was 300), up to
   3 lines, bubble bottom anchored y410, grows UPWARD. Streaming text
   keeps the companion cleaning rules (markdown stripped, clip 240).
2. Heard line: right-aligned `chip` above the caption (w = text + 24,
   max 300), Caption `dim`: "YOU: …" single line, ellipsis. It must
   never push the caption into the stage: caption zone is fixed.
3. Coach line: until the first completed PTT in the session, the
   caption zone shows Caption `dim`: "HOLD THE BUTTON AND SPEAK".
   Session-only flag; no NVS write.
4. New states as designed screens:
   - UNPAIRED (no Muse link configured): avatar in Off scheme,
     state word "NOT LINKED", caption card = pairing instructions
     (§3.8 P2 summary + "OPEN SETUP" pill → provisioning screen).
   - OFFLINE (Wi-Fi down): state word `warn` "OFFLINE", caption
     "WI-FI IS DOWN — RETRYING", avatar Ready scheme dimmed 50%.
   - ERROR: state word `danger`, caption = one-line error summary;
     no stack traces, no codes the user cannot act on.
5. Sleep-dim: nothing app-specific (shell dims).

```
+-----------------------------------------+  y0   SZ
| 3:27 PM                  [####] 100%    |
+ - - - - - - - - - - - - - - - - - - - - +
|              R E A D Y                  |  y50  state word
|         (agent status line)             |  y76  Caption
|          . - - ring R188 - - .          |
|        /      stage R138      \         |  avatar 256
|       |     [ Jolly avatar ]   |        |
|        \    ▂▄▆ meter 14seg   /         |  cy+96
|          ' - - - - - - - - '            |
|              [YOU: …chip]               |  heard chip
|        [ caption bubble 370w ]          |  ≤3 lines, base y410
|               ( PTT 72 )                |  y424–496
+-----------------------------------------+
```

### 3.5 Hermes app (app "hermes") — the big rebuild

Principles: (a) ALL text via draw_text_aa — the legacy scaled 5×7 path
is deleted from this app; (b) conversation = turns in bubbles, not a
text dump; (c) the shell backdrop shows (no full-screen flat fill);
the gold dialect comes from surfaces + gold type, not from painting
the whole screen; (d) the duplicate battery bar is deleted (the shell
status zone already shows battery).

Layout:
- y48–84 HEADER: "Hermes" Title 28 gold left (x20). Right: pill-status
  (bar fill): dot + word — LINKED `online` / PAIRING gold / CONNECTING
  gold / OFFLINE `dim` / AUTH FAIL `danger`.
- y92–112 SUB-ROW: Micro `dim` left: "UP 3H12" + (if queued) "· 2
  QUEUED"; right: battery % Caption (text only — no bar). Device ID
  moves to Settings → About (it is diagnostics, not conversation).
- y124–404 CONVERSATION (drag scroll, momentum, scroll thumb while
  dragging): turns oldest→newest, newest pinned to bottom on new
  content:
  - Hermes turn: gold bar bubble (existing hist style), x8 w394 r14:
    stamp Micro gold "2:32 PM" (12H per §2.1), reply Body 18 `text`,
    ≤6 lines visible in-bubble, longer replies scroll with the list.
  - User turn: slate bubble (§2.3), right-aligned, w ≤ 280: "YOU:"
    Micro dim + transcript Caption `text`, 1–3 lines.
  - Status row (busy): three 6 px dots gold, 300 ms stagger + Caption
    gold = the existing status text ("THINKING" default).
  - OTA row (when active): "UPDATING 42%" Caption gold + progress bar
    370×12 r6 (faint track, gold fill — fill_round_rect pair, exists).
- y412–424 NOTICE: Micro gold, 1 line (existing notice mechanism).
- y420–496 PTT ZONE: ptt-pill 370×64 gold: mic icon20 gold + Headline
  gold "HOLD TO TALK". Recording: pill fills gold, label `bg`-dark
  "LISTENING…". Idle+linked+empty history: a "SAY HELLO" quick pill
  (200×48, gold gloss — the existing hello bubble, AA label) floats
  centred at y≈300 instead of inside the list.
- Prompt state (replaces conversation, as today): prompt title
  Headline gold y170, body Body `text` wrapped, expiry Micro dim when
  the view provides it, YES/NO gloss bubbles 181×60 at y380 (kept —
  they meet the 56 px rule), labels Headline AA (dark on colour).
- Auth-failed state: danger card: error Body `danger` wrapped,
  instructions Body dim, "HOLD BOOT 2S TO FORGET + RE-ENROLL" — the
  2000 ms Hermes hold is preserved (shell escape is 3000 ms; no
  conflict, proven on hardware).

```
+-----------------------------------------+  y0   SZ
| 3:27 PM                  [####] 100%    |
+ - - - - - - - - - - - - - - - - - - - - +
| Hermes                    (• LINKED)     |  y48  Title + pill
| UP 3H12 · 2 QUEUED              72%     |  y92  Micro row
| [2:32 PM                        ]       |
| [ Hermes reply, Body 18, gold   ]       |  conversation
| [ bar bubble                    ]       |
|              [YOU: slate bubble ]       |
| ● ● ● THINKING                          |  status row
|                                         |
| [ (mic)      HOLD TO TALK       ]       |  y420 ptt-pill 64
+-----------------------------------------+  y502
```

States: EMPTY (linked, no turns): feather icon40 gold-dim centred
y≈230 + Caption "SAY HELLO TO HERMES" + the SAY HELLO pill. OFFLINE:
empty-state recipe — wifi-off icon40, Title "NO LINK", Caption "The
watch joins Wi-Fi and connects to the Hermes hub automatically."
(+ error line `danger` Caption when the view carries one). PAIRING:
§3.8 P1. CONNECTING: sub-row word + status row dots + "CONNECTING…".

Renderer needs: shared pixel-width wrap helper (§4 R1) — Hermes's
existing draw_wrapped measures with the legacy scale metrics and must
be re-based on text_width_aa. Everything else exists.

### 3.6 Notifications (app "alerts")

Keep the audited card list (x20 w370, list y56, gap 12, bell chip
40×40, title Body SemiBold, body Caption dim pixel-wrapped, receipt
Micro faint right, drag scroll, tap-expand, swipe-right dismiss,
store cap 16). Additions:

- Header row y52: "ALERTS" Headline caps left; right Micro dim
  "3 NEW" (unread count; blank when 0). List starts y92 when the
  header shows (shift the list origin; cards unchanged).
- Empty state (store empty): empty-state recipe — bell icon40
  `faint`, Title "ALL QUIET", Caption "Hermes prompts and Muse
  notices land here." No pill (no action exists).
- Actionable cards (Hermes-prompt-backed): card grows a button row:
  two 48 px mini-buttons (YES: `ok`-tinted gloss; NO: `danger`
  outline), labels Caption SemiBold; expiry line Micro `warn`
  "EXPIRES 0:42" above the buttons (expiry rendering already exists
  in the store; surface it on the card, not only in-app).
- DND banner: when DND is on, a slim card (h 48) pins at list top:
  moon icon20 + Caption "Do Not Disturb is on — alerts won't wake
  the watch." It scrolls away with the list (it is information, not
  chrome).
- Scroll thumb: 4 px wide, right edge x402, shown ONLY while
  dragging (Wear OS WO-V8 pattern, §1.2); thumb length ∝ visible
  fraction, min 24 px (v1.0 token).
- States: loading/error — n/a (local store). Sleep-dim — shell.

```
+-----------------------------------------+  y0   SZ
| 3:27 PM                  [####] 100%    |
+ - - - - - - - - - - - - - - - - - - - - +
| ALERTS                            3 NEW |  y52
| [(bell) Hermes prompt        2:31 PM ] |  cards w370
| [       Body preview, Caption dim    ] |
| [       EXPIRES 0:42                 ] |
| [       [ YES ]        [ NO ]        ] |  48px buttons
| [(bell) Muse notice          1:04 PM ] |
| [       …                            ] |
+-----------------------------------------+
```

### 3.7 Weather (app "weather")

The current screen is already token-clean; this is a polish spec.
Keep: wx icon64 y76 (condition-tinted), hero temp Display 96 y150,
condition Title y262, two cells y330 (HUMIDITY / UPDATED, 179×76),
freshness line y428.

Changes:
1. Freshness becomes a pill-status centred y424: `ok` dot + "LIVE",
   or `warn` dot + "STALE · 42 MIN" (age from fetched_epoch; the
   stale threshold stays one refresh interval). When stale, the hero
   temp draws in `caption` instead of `textNum` (data you cannot
   trust is visually quieter — one rule, easy to review).
2. Tap anywhere = refresh now. While fetching: pill shows
   "UPDATING…" (gold-neutral `dim` dot pulsing via the palette
   cross-fade token), hero unchanged. Failure: keep last data, pill
   flips to STALE. Weather is read-only data; a tap is a request,
   not an edit — no button chrome needed.
3. Empty state splits by reason (the service already distinguishes
   them): OFFLINE → wifi-off icon; CONNECTING → cloud icon + "…";
   FETCH FAILED → alert icon; NO DATA YET → cloud icon. Headline
   stays "NO DATA YET"; the Caption carries the reason (existing
   strings). Icon is the only change per reason.
4. Station line: IF the weather service exposes a station id, a
   Caption dim line under the condition ("STATION KFFO"-style,
   placeholder id only in this doc). If it does not, the line is
   omitted — the design does not invent a data field. Hi/lo, wind
   and hourly are NOT designed: the snapshot does not carry them
   (§6). If the service grows them later, they slot into a second
   cell row at y418 and the pill moves to the hint zone.

```
+-----------------------------------------+  y0   SZ
| 3:27 PM                  [####] 100%    |
+ - - - - - - - - - - - - - - - - - - - - +
|                ( wx 64 )                |  y76
|                  72°                    |  y150 Display 96
|               Clear Sky                 |  y262 Title
|   [ HUMIDITY 41% ] [ UPDATED 2:15 PM ]  |  y330 cells
|              (• LIVE)                   |  y424 pill
|           TAP TO REFRESH                |  y458 hint Micro
+-----------------------------------------+
```

### 3.8 Provisioning / pairing screens

Three flows exist in this product; each gets a designed screen. No
screen ever displays a credential, token or code that is not the
flow's own public artifact (privacy rule).

P1 — Hermes pairing (state inside the Hermes app, §3.5):
- Gold card x20 y140 w370 r18 (bar bubble): label Micro dim centred
  "PAIRING CODE" y164; the 8-char code in Pair 40 Bold gold centred
  y196 (the Pair face exists for exactly this); Caption centred
  y268 "APPROVE ON THE HERMES HOST:"; the approve command wrapped
  Caption `text` centred inside an `input`-fill chip y300 — shown
  with a PLACEHOLDER code in all docs/screenshots, never a live one.
- Bottom: spinner arc (arc primitive, 300°/s) + Micro dim "WAITING
  FOR APPROVAL…". Success → pill-status LINKED + auto-return to
  conversation after 2 s. If the hub rotates the code, the screen
  simply shows the new code (the view already re-reads per frame).

P2 — Muse pairing (new "setup" surface in the Muse app, shown when
unpaired; backs the BLE Link setup flow):
- Blue dialect. Title 28 "PAIR WITH MUSE" y64. Steps as three
  Caption rows y120–200: "1 · OPEN THE MUSE APP ON YOUR PHONE",
  "2 · SETTINGS → DEVICES → ADD DEVICE", "3 · PICK THIS WATCH:".
- Name chip centred y224 (chip, Headline): "MuseGadget-XXXXXX"
  (XX = the device's own suffix; a placeholder pattern in docs).
- State WAITING: spinner arc + Micro "WAITING FOR THE APP…".
- State CONFIRM (phone connected, awaiting the presence press):
  Headline accent "CONFIRM PAIRING" + Body "HOLD BOOT TO CONFIRM" +
  hold-progress arc that fills across the confirm hold (the press is
  PHYSICAL ONLY — MDB may observe, never inject; that boundary is a
  security property, not a missing feature).
- State DONE: check icon40 `ok` + Title "LINKED" + Caption "You can
  put the phone away." Auto-advance to the Muse stage after 2 s.
- State FAILED/TIMEOUT: `danger` Caption reason + "TRY AGAIN" pill
  (56 px) → back to WAITING. BLE then shuts down per the SDK flow;
  steady-state cost of this screen is zero.

P3 — First-run / no-Wi-Fi (shell-level card; auto-shown on the clock
face when NetConfig is empty, and reachable from Settings → About):
- Empty-state recipe: wifi-off icon40, Title "NO WI-FI YET", Caption
  "This watch joins Wi-Fi during Muse pairing, or from a setup
  string over USB." No keyboard, no QR (§6) — provisioning routes
  are the BLE flow (P2) and the wired setup string, and the screen
  says exactly that.

P4 — Forget / unlink confirm (shared dialog, §2.2 `dialog`):
- Scrim + card 330 wide centred: Title "FORGET HERMES?", Body
  consequence ("This watch will unpair and ask for a new code next
  time it connects."), buttons 56 px: "KEEP" (bubble) / "FORGET"
  (danger gloss). Used by the Hermes auth-fail recovery and any
  future forget row in Settings. BOOT-hold paths stay as the
  no-touch fallback that reaches the same action.

---

## 4. Renderer / OS additions the design needs (each costed)

Nothing in §3 requires a new drawing capability beyond R2. R1/R4/R6
are plumbing; R3/R5 are optional. Estimates are engineering estimates
to be verified at the stage gate (image size + DIRAM deltas).

- R1 — Shared AA wrap helper. `wrap_text_aa(text, face, max_width)`
  in font.cpp: pixel-width greedy wrap returning line spans, so apps
  stop re-implementing wrap (Hermes's legacy-scale wrap is the last
  one). Flash +~0.4 KB, DRAM +0 (caller-owned span buffer, ≤32 spans
  on the stack/PSRAM per existing practice). Needed by: §3.5, §3.4.
- R2 — `fill_rect_blend(x,y,w,h,color,alpha)` on Canvas: per-pixel
  blend fill (put_pixel_blend already exists; this is its rect
  loop). Flash +~0.2 KB, DRAM +0. Needed by: §2.2 dialog scrim.
  (Banding note: scrims repaint rarely; the 4-row gradient banding
  precedent applies — flat blend, no LUT.)
- R3 (OPTIONAL) — Two icon glyphs: bluetooth + bolt, in icon20 and
  icon40 faces (re-bake via tools/gen_font.py). Assets +~1.5–3 KB
  flash total (budget after: ~164 KB of the 256 KB law), DRAM +0.
  Bluetooth serves P2/pill contexts; bolt serves the clock CHARGING
  line. Both specs work without R3 (text fallbacks specified).
- R4 — StatusSnapshot service (OS, not renderer). A small struct
  (~120 B static) the shell fills from EXISTING seams — net state,
  Muse link state, hermes_view(), weather snapshot, notification
  unread count, power — plus a `status_snapshot()` accessor apps
  read per frame (cheap field reads; the registered status-provider
  seam used by Settings About is the precedent). Flash +~0.6 KB,
  DRAM +~120 B static. Needed by: §3.1 chips/next-line, §3.2 badge.
  This is the single structural change in the overhaul: today the
  clock app can see none of this data.
- R5 (OPTIONAL, LAST) — Slide-lite transition. Canvas gains a
  translate+clip offset (state ~16 B, code +~0.5 KB flash); shell
  drives a 220 ms / 6-frame horizontal slide for launcher↔app only,
  dip stays for all other switches. Risks: band-flush dirty hashing
  must key on translated output; apps cache no absolute coords
  today (all drawing goes through the framebuffer), but the Muse
  ring geometry must be verified under translation. NO second
  framebuffer (that would be +411,640 B PSRAM — affordable — but
  the compositor pass costs a full-frame copy per frame; rejected
  as poor value for a 220 ms nicety).
- R6 — Settings persistence (OS). NVS namespace keys for brightness,
  volume, DND, time format, watch face, sleep timeout; load at
  boot before first render; save on change (single commit per
  change, the net_config save pattern). Flash +~0.8 KB, NVS ~64 B,
  DRAM +~40 B. Needed by: §3.3. Also the consumer wiring that makes
  DND real (notification wake/banner gate) and the time-format read
  in the shell clock + all stamp formatters.

Explicitly NOT needed: second framebuffer, runtime blur (banned by
v1.0), image decoders, a text-entry keyboard, a QR encoder, LVGL.

---

## 5. Build order — independently shippable stages

Every stage: host tests for any pure logic touched, root suite green,
size+DRAM gate vs the stage-0 baseline, flash, then acceptance =
`mdb screenshot` dumps of the listed states, visually inspected by
the parent against §3 wireframes BEFORE the stage is called done.
A stage that fails inspection does not merge forward.

- Stage 0 (PREREQUISITE — owned by the hardware wave, not this
  design): fonts load on glass (`assets load: 11/11` in the boot
  log), touch input works, MDB answers (`mdb health`, real
  screenshot PNG). Nothing in this doc can be verified on glass
  until Stage 0 lands; host-side work for Stages 1–3 may proceed
  in parallel but cannot be accepted.
- Stage 1 — Clock face (§3.1) + R4 + R6's time-format key only.
  Screenshots: normal / stale weather / offline / charging /
  sleep-dim / analog. Acceptance detail: 12-hour with AM/PM, all
  three chips live, next-line priority order demonstrated by
  toggling states via MDB where the tooling allows.
- Stage 2 — Hermes rebuild (§3.5) + R1. Screenshots: conversation
  with history / thinking / user+reply turn pair / prompt YES-NO /
  pairing (P1) / offline / auth-failed / empty. Acceptance detail:
  ZERO legacy 5×7 text anywhere in frame (checked visually — the
  dot-matrix texture is unmistakable); stamps in 12H.
- Stage 3 — Launcher (§3.2) + Settings (§3.3) + R6 (full) + DND
  consumer. Includes Light mini-app. Screenshots: launcher (DND
  off/on), settings main, about, light. Acceptance detail: reboot
  the watch via MDB between two settings screenshots — values must
  persist; DND on → a posted notification must NOT wake the panel
  (verified by screenshot staying dim + the card present after
  manual wake).
- Stage 4 — Muse polish (§3.4) + Notifications (§3.6).
  Screenshots: Muse ready / listening (arc) / thinking (spinner)
  / speaking (ring empty, meter live) / error / unpaired;
  notifications empty / list / actionable with buttons / DND
  banner / mid-drag thumb. Acceptance detail: caption at full
  370 width with a long streamed reply; heard chip never overlaps
  the stage.
- Stage 5 — Weather polish (§3.7) + Provisioning (§3.8 P2–P4) + R2.
  P2 ships WITH the BLE pairing wave (it is that wave's UI).
  Screenshots: weather live / stale (hero quieted) / updating /
  empty-offline / empty-failed; P2 waiting / confirm / done /
  failed; P4 dialog. Acceptance detail: P2 confirm state reached
  with a real phone-side attempt during the pairing session.
- Stage 6 (OPTIONAL) — R5 slide-lite + R3 glyphs. Screenshots:
  mid-transition frame + full regression set of every screen
  above. Ship only if Stages 1–5 are all accepted; the dip is a
  fine permanent answer if this never lands.

---

## 6. Deliberately NOT copied — and why

- LVGL / widget-tree UI (InfiniTime, ZSWatch, AmoledOS, gadget-206):
  stack mismatch by design. AIWatchOS's native canvas + band-flush
  exists precisely to fit 512 KB SRAM discipline; importing LVGL
  would add its draw buffers and object model and invalidate the
  flush architecture. We copy their LAYOUTS, never their toolkit.
- AsteroidOS's 4-way spatial swipe map: our shell replaced edge
  swipes with one large bottom target + BOOT-hold escape after
  edge gestures conflicted with in-app drags (scrolling lists,
  sliders). Reintroducing it would re-break Settings sliders and
  Notifications drag-scroll. One directional idea survives:
  everything is ≤1 gesture from the face.
- AsteroidOS watchface/wallpaper gallery + face-wallpaper pairing:
  needs image decode + storage management we do not have; a single
  house style (digital + analog) covers the need at this app count.
- AsteroidOS always-on full face: AMOLED burn-in + battery. We take
  the nightstand LESSON (charging state is face-level, large) and
  the dim-ladder shape (Infineon example) instead: time-only at
  brightness 15 while idle.
- Honeycomb launcher (Apple Watch pattern): solves a 30+ app
  catalogue with small, dense targets. We have 7 apps; a 3×2 grid
  of 112 px tiles beats it on every target metric.
- On-watch keyboard (gadget-206 ships one for token entry):
  credential entry on glass is a one-time task with the worst
  possible input method, and anything typed is one MDB screenshot
  away from exposure. Provisioning stays: BLE from the Muse app
  (P2) or the wired setup string (P3 says so on screen).
- QR-code pairing (Chronos-Watchy's pattern): our flows have no
  payload a QR would carry — Hermes uses an 8-char human code
  (Pair 40 renders it better than any module grid), Muse uses
  BLE ECDH with nothing human-readable at all. A QR encoder
  (+flash, +a grid renderer) buys nothing.
- Steps / heart-rate complications and health apps (all
  references): no step or HR service exists in AIWatchOS today.
  The chip row (§3.1) has room for a fourth chip the day a real
  IMU service lands; until then, drawing the data would be lying.
- "Next event" complication: no calendar data reaches the watch
  (InfiniTime's ecosystem hit the same wall — it needs a phone-side
  piece). The slot carries the alerts summary instead (§1.1).
- Music control screens (InfiniTime, LVGL demo): no music data
  source or sink pairing exists in this stack.
- Pebble-style timeline / cards-as-primary-surface: our cards
  already live where they belong — inside Notifications and the
  Hermes display-card path — not as a parallel home surface.

---

## 7. Open decisions (flagged, not blocking the design)

1. DND vs Hermes prompts (§3.3 row 3): spec says prompts still
   wake under DND. If Duckets wants total silence, the change is
   one gate in the notification consumer — but approvals would
   then expire unseen, which is why the spec defaults as it does.
2. Analog face complications: spec keeps the analog dial clean.
   If he wants parity, the chip row fits under the dial at the
   cost of dial radius (R188 → ~R150); not recommended.
3. Status-zone time in 24H when the face is 12H (or vice versa):
   spec says the setting applies everywhere (§2.1). One format,
   no exceptions, is the legible choice.
4. The "Apps" launcher tile removal (§3.2) assumes no hidden
   second app page exists today; if a future app needs a home
   before the pagination rule triggers, it takes the Clock tile's
   slot discussion, not a new tile size.

---

## 8. Sources (read 2026-10-13)

External:
- [S1] CNX Software — "AsteroidOS 2.0 open-source smartwatch OS
  released" (QuickPanel, launcher styles, nightstand, weather
  legibility overhaul): https://www.cnx-software.com/2026/02/18/asteroidos-2-0-open-source-smartwatch-os-released-now-supports-around-30-devices/
- [S2] Android Police — AsteroidOS 1.0 review (spatial swipe
  navigation, minimal-animation philosophy):
  https://www.androidpolice.com/2018/06/10/asteroidos-1-0-promising-open-source-smartwatch-os-plenty-room-improvement/
- [S3] arteeh — PineTime UI design proposal (centre-out map, top
  bar inside grid/notifications, 6-per-page app grid):
  https://github.com/arteeh/pinetime
- [S4] Infineon — PSoC Edge LVGL smartwatch example (three power
  states, simplified always-on UI after 30 s idle, swipe pages):
  https://github.com/infineon/mtb-example-psoc-edge-gfx-lvgl-smartwatch
  and LVGL forum — the official LVGL smartwatch demo thread:
  https://forum.lvgl.io/t/how-to-obtain-the-source-code-for-the-official-perfect-smartwatch-gui-demo/17160
- [S5] Android Developers — Wear OS app quality (WO-V2 48×48 dp
  targets, WO-V3 swipe-to-dismiss, WO-V5 state preservation,
  WO-V8 scrollbar while scrolling):
  https://developer.android.com/docs/quality-guidelines/wear-app-quality?authuser=3
- [S6] SQFMI — Watchy watchface creation docs:
  https://github.com/sqfmi/watchy-docs/blob/HEAD/docs/create-watchface.md
- [S7] crispface — Watchy firmware with composable complications
  and per-complication refresh cadences:
  https://github.com/chrispyfur/crispface
- [S8] fbiego — Chronos-Watchy (default face = time + phone
  battery + last sync; QR pairing pattern — considered, §6):
  https://github.com/fbiego/chronos-watchy
- [S9] CNX Software — "InfiniTime 1.0 firmware released for
  PineTime" (two faces, quick-action menu: brightness / DND /
  flashlight; settings: timeout, wake, time format, watchface):
  https://www.cnx-software.com/2021/04/22/infinitime-1-0-firmware-released-for-pinetime-smartwatch/
- [S10] AmoledOS — ESP32-S3 AMOLED watch firmware (settings list,
  AMOLED full-white flashlight, thumb-sized calculator keys,
  notification history with actions):
  https://github.com/charliejgallo/ESP32S3_AmoledOS
- [S11] ZSWatch — Apps wiki (app-picker / application-manager
  pattern; every app must be able to close itself):
  https://github.com/ZSWatch/ZSWatch/wiki/Apps

In-tree / in-family:
- AIWatchOS docs/ui/DESIGN_TOKENS.md ("Muse Gloss v1.0"),
  docs/ui/IMPLEMENTATION_PLAN.md, FEATURE_PORT_MATRIX.md,
  RESOURCE_BUDGET.md, docs/mdb.md.
- AIWatchOS source read for §0.3/§0.4: components/aiwatchos_os
  (app.hpp, font.hpp, colors.hpp, clock_face.cpp, launcher_ui.cpp,
  settings_page.cpp, notifications.cpp, weather_app.cpp),
  main/app_main.cpp, apps/muse muse_adapter.cpp,
  apps/hermes hermes_adapter.cpp.
- muse-gadget-206 (same panel): esp32/components/muse/muse_ui.c —
  ring Ø402, caption wrap 256 px, meter 14×9 px gap 4, 300 ms
  reply-expand animation; Montserrat ladder 12/14/16/20/28.
- Watch UI audit (~/watch-ui-audit/FEATURE_PORT_MATRIX.md) for the
  shipped-screen inventory this overhaul revises.

END OF DESIGN DRAFT — uncommitted, for parent review.
