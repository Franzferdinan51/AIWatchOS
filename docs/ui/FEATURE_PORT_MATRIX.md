# FEATURE PORT MATRIX — phone apps → AIWatchOS watch apps

Analysis only, 2026-10-13. Directive (Duckets): use the two phone apps as the
enhancement source for the watch apps — MuseCompanionApp → watch Muse app,
HermesDesktopBuddy → watch Hermes app — porting as much as works or is useful,
under the watch's resource restraints and goals (glanceable, voice-first, same
products' feel: Muse Gloss blue for Muse; Hermes keeps indigo/gold).

## Sources inventoried (exact states)

- **MuseCompanionApp** — inventoried at local `develop` commit
  **`8e4656185c0046df938e222e9b3c454a0d05a2da`** (2026-10-07, "Add 7 agent
  capabilities: files, scenes, sensors, media, cards, vision, home"),
  tree `~/proj/MuseCompanionApp` read exactly as it sits. Pushed `main`
  (`7b3bafd`, 2026-10-04) is OLDER — the local develop tree is the freshest
  feature source and is what this matrix rates.
- **HermesDesktopBuddy** — inventoried at pushed `main` commit
  **`7d8b6b6dbe2f3cbd01206a2934c80874c7503d8b`** (2026-10-11) from a fresh
  clone at `~/proj/HermesDesktopBuddy`. STRICTLY read-only; nothing written
  to that repo. The `/Volumes/Sabrent/Minta/HermesDesktopBuddy` copy is not a
  git repository at all; a file diff shows its sources byte-identical to the
  inventoried main except 2 PNGs in `res/drawable-nodpi/` absent from main —
  no feature code was read from it.
- **⚠️ REMOTE MOVED DURING THIS AUDIT.** Re-check at the end of the audit
  (2026-10-13, ~11:25 EDT): HermesDesktopBuddy `main` HEAD is now
  **`31dcd572df8b017f38337f743d3d580b34950ff4`** — one new commit, "Fix
  connection + build breakers, Muse-style UI overhaul, official Nous Girl
  art". Delta assessed and folded in (rows marked ★31dcd57): user-visible
  connection-error hints, backoff that only resets after a completed
  handshake, message timestamps, long-press copy on bubbles, dashboard
  battery %, settings validation + auto-reconnect on host change, FGS
  special-use subtype, glossy Muse-style bubble art + Nous Girl avatar.
  **No protocol/frame changes** — no verdict below flips because of it; two
  rows (H19, H13) gain a ★ match and one (H21) is new.
- **Watch targets rated against:** AIWatchOS Muse surface from
  `~/aiwatchos-net` (branch `net-muse-transport`: PTT/outbox/account API/
  Noise link) and Hermes surface from `~/aiwatchos-hermes` (branch
  `hermes-client`, in flight: full protocol-v1 core — pairing, text, prompt,
  display/image cards, audio sinks, OTA), on the integrated tree being built
  in `~/aiwatchos-integrate`.

## Transport truths that decide most verdicts

- **Watch Muse link ≠ companion's Link.** The companion is a Link *device*:
  it registers `commands_v2` and executes `link.invoke`s (that's how
  `companion.set_status`, `display.show_card`, `display.draw_url`,
  `prompt` approvals reach it) plus BLE pairing v5. **AIWatchOS speaks none
  of that.** Its Muse link is the hatch chat surface only: HTTPS
  `POST /chat/stream` (voice notes up) + long-lived `POST /chat/subscribe`
  NDJSON over TLS+Noise XX. Proven on that stream today (parser in
  `muse_link.cpp::on_sub_line`): `delta.message_start/text_append/
  message_done`, `message.assistant`, `agent.status`, `task.status`.
  Everything else host→watch (cards, images, notices, prompts) would need
  the host to emit a chat-stream event that is **not proven to exist** —
  those rows carry dependency **H-EVENT** and are ADAPT at best until the
  host event vocabulary is verified. Replies are TEXT by design on both
  sides (`output_modality:"text"`); there is no host audio-reply stream.
- **Watch Hermes link = gadget protocol v1** to the Mac hub
  (`ws://192.168.1.96:8765/gadget`, HMAC auth, pairing codes) — frame set:
  text / audio (PCM16 ch1) / prompt / display / image (RGB565 ch2) /
  action / OTA (ch3) / state / cancel / session.new / heartbeat. The hub
  pre-shapes reply text (markdown stripped, ASCII-folded) and speaks
  replies (`speak_replies:true`) to devices declaring a speaker. This is
  the SAME protocol HermesDesktopBuddy speaks — Hermes verdicts are
  therefore mostly about UI/state, not transport.
- **Resource law** (RESOURCE_BUDGET.md + DESIGN_TOKENS.md §11): DRAM is
  TIGHT (~50KB free at worst spike; TLS handshake is the peak event —
  **never two concurrent TLS sessions**; the subscribe session is
  persistent, so any extra HTTPS must be sequenced, never parallel).
  PSRAM is plentiful (~7.5MB free) — histories/buffers live there. Assets
  ≤256KB flash in the FAT partition; UI SRAM ≤4KB; glyph cache ≤64KB
  PSRAM. No keyboard: anything needing text entry, big lists, or heavy
  media gets an honest SKIP.
- **UI phase refs** are to `IMPLEMENTATION_PLAN.md` (same folder):
  Ph 1 = framebuffer primitives + band flush + touch Release/Move;
  Ph 2 = AA fonts/icons; Ph 3.3 = Muse face; Ph 3.5 = notifications;
  Ph 3.7 = Hermes screen render.

## Verdict + cost legend

- **PORT** — build it on the watch essentially as the phone does it.
- **ADAPT** — the value survives, redesigned for touch/PTT/buttons only.
- **SKIP** — do not build (reason stated, one line).
- **MATCHES** — watch already has it; listed so nobody rebuilds it.
- Cost class = flash+RAM footprint of the watch work: **S** small,
  **M** medium, **L** large.

---

# PART 1 — MuseCompanionApp → AIWatchOS Muse app

Watch Muse today: PTT (BOOT + bottom touch strip, 30s cap) → ADPCM→WAV
voice note → outbox FIFO drain when linked → latest reply text only
(4KB cap) in `muse_link_last_reply()`; `agent.status`/`task.status`
events are received but only used for settle timing (payload discarded);
turn ring of 4 (start/duration/bytes, no outcomes, no text); mic level +
progress; MP3 decode/play path (`muse_play_reply`, minimp3) with nothing
feeding it; link states shown as text + a 3px strip.

| # | Phone feature (what it does) | Verdict | Watch design / transport / cost / dependencies |
|---|---|---|---|
| M1 | **Live streaming reply** — delta events append into the bubble as the agent writes (`chat.dart applyServerEvent`) | **PORT** | The deltas are already accumulated mid-turn in the link's TurnRx; expose the in-progress text and render it growing in the caption. Transport: existing subscribe stream. Cost **S**. Dep: UI Ph 3.3 caption. |
| M2 | **Agent status line** — `agent.status`/`task.status` + tool activity codes → friendly text ("Searching…", "Taking a photo…"), drives pose (`agent_status.dart`, `captions.dart activityCaption`) | **PORT** | Parse the payload the parser currently throws away; map codes through a port of the `activityCaption` table; feed the state word + status line. Transport: existing events. Cost **S**. Dep: UI Ph 3.3 state word/ring. **Best value-to-cost in this whole matrix — the data is already on the wire.** |
| M3 | **Caption cleaning** — reply → caption: strip code fences/links/markdown chars, collapse whitespace, clip 240 chars (`captions.dart`) | **PORT** | Port the cleaning function verbatim (pure text, host-testable). This is also the watch's entire markdown strategy (see M18). Cost **S**. Dep: none. |
| M4 | **Reply as the face's caption bubble** — finished reply lands under the avatar, styled, persistent until next turn | **PORT** | Already specced as UI Ph 3.3 (caption bubble wired to the reply); this row records that Ph 3.3 IS the port of the companion's core surface. Cost **M** (inside Ph 3.3). Dep: UI Ph 2 fonts, Ph 3.3. |
| M5 | **Heard-transcript bubble** — server patches the voice-note bubble with what STT heard (`message.user`) | **ADAPT** | Parser add: handle `message.user` → keep last transcript; render as a dim bubble above the reply ("You: …"). No keyboard means no edit-and-resend — display only. Transport: subscribe stream (event exists in the protocol family; parser ignores it today). Cost **S**. Dep: UI Ph 3.3. |
| M6 | **Conversation history** — 100-msg bubble list, persisted 200, search/pin/archive | **ADAPT** | Last ~4 exchanges (transcript+reply pairs) as a scrollable bubble stack, PSRAM ring, session-scoped. Persistence, search, pin, archive: SKIP (flash wear, no keyboard, glanceable-first). Cost **M**. Dep: M1/M3/M5, UI Ph 1.7 touch Move for scroll. |
| M7 | **Choice cards with buttons** — agent asks, user taps one of ≤4 buttons, choice recorded (`display.show_card`, choice_cards.dart) | **ADAPT** | Watch side is buildable (card recipe in tokens; mirror the Hermes client's card code) but the channel is not: cards reach the companion via Link invoke / link push, which the watch doesn't speak. Needs **H-EVENT** (host emits a card event on /chat/subscribe) + a choice-return convention (text item on /chat/stream). Cost **M** watch-side + host work. Dep: H-EVENT, UI Ph 3.3-era card render. |
| M8 | **Server display cards (button-less)** — VM `display` message → card on stage | **ADAPT** | Render-only subset of M7; if the host emits any display/card event on the chat stream this is the cheapest card win. Cost **S–M**. Dep: **H-EVENT**. |
| M9 | **Character image from the agent** — `display.draw_url` + first image URL in a reply auto-drawn on stage, cached across restarts | **ADAPT** | `draw_url` itself: SKIP (Link-only). The reply-URL path works on watch transport: scan `last_reply` for an image URL (companion does exactly this), HTTPS GET **sequenced while no turn/TLS handshake is in flight** (one-TLS rule), JPEG decode (tjpgd-class) into a PSRAM buffer, cover-crop into the round stage. Cost **L**. Dep: JPEG decoder component, UI Ph 3.3 stage. |
| M10 | **Spoken replies (cloud TTS)** — reply spoken via Android TTS or OpenRouter (`openrouter_tts.dart`), speech volume, barge-in stop | **ADAPT** | No host audio exists and no on-device engine (M11); the watch's answer is the companion's opt-in path: POST cleaned reply text (chunked ≤1800 chars) to OpenRouter speech → MP3 streamed into the **already-vendored** minimp3 `muse_play_reply()`; speak on/off + volume from OS Settings; tap avatar / BOOT = stop (barge-in). Transport: HTTPS, sequenced TLS. Cost **M**. Dep: **Duckets' TTS decision + API key in provisioning** (setup-string field); UI Ph 3.3 SPEAKING state. |
| M11 | **Native on-device TTS** (Android TTS voices, voice picker) | **SKIP** | No TTS engine exists for this target in-tree; M10 is the watch's TTS. A voice picker is meaningless until M10 ships (then voice id = one provisioning field). |
| M12 | **Wake word** — on-device openWakeWord, "Hey Muse" | **SKIP** | The companion's bundled model is itself a `hey_jarvis` STUB (no Hey Muse model exists); TFLite wake-word on the S3 is a new ML workstream + always-on-mic power against a TIGHT budget. BOOT/PTT is the watch idiom. Revisit only if a real model is ever trained. |
| M13 | **Live mode** — hands-free loop: VAD listen → 2s silence sends → think → speak → listen… (`live_mode.dart`) | **ADAPT** | "Conversation mode" toggle in the Muse app: RMS VAD from the adapter's existing level ballistics ends the turn after ~2s silence (auto-release), and after the reply (or TTS, M10) finishes it listens again; 5-min no-speech exit, BOOT exits anytime. Cost **M**. Dep: M10 for the full loop; works text-only without it. |
| M14 | **PTT voice notes** — hold to talk, WAV note, server transcribes | **MATCHES** | Watch has it end-to-end (BOOT + touch strip, 30s cap, outbox). No work. |
| M15 | **Outbox for failed sends** — FIFO, persisted, flush on reconnect, stop at first failure | **MATCHES** | Same rule both sides (the watch's drain seam was ported from it). Persistence depth differs (watch outbox is in-PSRAM session scope) — acceptable on a watch. No work. |
| M16 | **Quick-reply chips** — "Take a photo / Say it again / What can you do?" | **ADAPT** | Two canned text prompts as tappable pills under the caption ("What can you do?", "Say it again" — the latter only with M10). Needs a text-item send on /chat/stream (companion's `buildChatRequest` has the shape; the watch link sends voice notes only today). "Take a photo": SKIP (no camera). Cost **S–M**. Dep: text send in `muse_link`. |
| M17 | **Composer, slash commands, share intake, chat search/pin/archive UI** | **SKIP** | Keyboard-and-phone idioms end to end. Voice replaces the composer; there is nothing to share *from* a watch. |
| M18 | **Rich markdown** — sortable tables, code blocks, Mermaid, KaTeX, callouts, task lists | **SKIP** | Replaced wholesale by M3's caption cleaning. Tables/diagrams/math are phone-screen content; rendering them at 410×502 buys nothing and costs the entire asset budget. |
| M19 | **Avatar pose/life system** — poses (idle/listening/thinking/speaking/error/boot/off), state schemes, blink/gaze/sparkles, pet tap; 3D + video stages | **PORT** (2D core) | The pixel stage is the canonical one (the gadget firmware mirrors the same 64-grid art + FIXED palette + state schemes, all specced in DESIGN_TOKENS §2/§3): bitmap-blit renderer + pose mapping from M2's status + blink/breathing; pet tap = happy hop through the existing touch path. 3D GLB + video stages: SKIP (no GPU, asset budget, and the pixel stage is the identity). Cost **M**. Dep: UI Ph 2, Ph 3.3 (this row is Ph 3.3's avatar half). |
| M20 | **VM selection + token lifecycle** — fetch_vms, default-or-wanted VM, ~4h token rotation | **MATCHES** | Net branch already does account resolution + wanted/default selection; neither side has switching UI. No work. |
| M21 | **Diagnostics view** — link state/detail, identity, registered commands, copyable dump | **ADAPT** | One info page in the Muse app: VM name, link state, last error, turn count, uptime (all data already exists in the link/adapter). The copyable-dump part: SKIP (nothing to paste into). Cost **S**. Dep: none. |
| M22 | **Provisioning status** — is the watch configured (token present, VM set, WiFi set), pairing state | **ADAPT** | Status rows from `net_config` booleans only — never render secrets. Credential *entry* stays out-of-band (setup string / NVS / build config): no keyboard, and BLE provisioning is the gadget firmware's world, not the OS's. Cost **S**. Dep: none. Pairs with the arrival flash checklist. |
| M23 | **`device.health` agent query** — agent asks the device for battery/model/OS | **SKIP** | Link-invoke only; no path on the chat link. The same data already sits on the watch's own status bar for the human. |
| M24 | **VM-pushed settings** (`companion.set_display`: theme, keep_screen_on, speak_replies) | **SKIP** | Link-only channel. Local equivalents already exist (OS Settings rows; display-sleep timeout from the power pass). |
| M25 | **Tool-approval prompts** — agent asks Allow once/session/always before phone tools (`approval_service.dart`, `prompt` messages) | **SKIP** (Muse app) | Approvals reach the companion over Link `prompt` messages; the watch Muse link has no prompt channel and the watch executes no agent tools. On this watch, approvals live in the **Hermes** app, whose protocol has native prompt frames (Part 2, H9). |
| M26a | **Agent-body tool surface** — `phone.*` (calls/SMS/clipboard/apps/screen control…), `usb.*` (OTG + serial), `file.*` workspace, `scene.*`, `vision.*` (Oct 7 capabilities) | **SKIP** | These make the *phone* the agent's hands: the watch has no camera, no USB-host role in the OS, no workspace, no telephony. Nothing here survives contact with a 410×502 voice endpoint, and the phone still does all of it. |
| M26b | **Local-AI stack** — LM Studio client + on-phone agent loop, SystemOne tool routing, agent-memory KV + Memory screen | **SKIP** | The watch is a thin endpoint to the VM by design; local inference/routing on an S3 is impossible and pointless. Agent memory for the watch's Muse lives server-side already. |
| M26c | **Media queue + Canvas docs** — audio queue player, persistent markdown/code docs with revisions | **SKIP** | No media-library role and no reading/editing surface on a watch. (One borrowed idea only: if M10 ships, its sequential chunk playback reuses the queue's play-next discipline internally — an implementation note, not a feature.) |
| M27 | **Server notices → notifications** — VM `notice` one-liners | **ADAPT** | In the companion this stream is defined but **unconsumed (STUB)** — it surfaces nowhere even on the phone. On the watch the sink exists and is empty: OS Notifications app + `add_notification()` with no live producer. If the host emits notice events on the chat subscribe, parser→notification is small. Cost **S**. Dep: **H-EVENT**, UI Ph 3.5 (list/wrap fixes). |
| M28 | **Voice-note / photo bubbles + Media tab** — replay sent notes, photo grid, resend | **SKIP** | Notes are not retained after the outbox drains; there is no photo source. The Media tab is a phone-gallery idiom. |

**Muse totals: 30 features rated — PORT 5 · ADAPT 11 · MATCHES 3 · SKIP 11.**

Already-matched (do not rebuild): **M14 PTT voice notes, M15 outbox FIFO,
M20 VM selection/token lifecycle** — plus, outside the companion's list,
the OS status bar already shows battery/link locally (the human-facing
half of `device.health`).

---

# PART 2 — HermesDesktopBuddy → AIWatchOS Hermes app

Phone app at the inventoried main is a *thin* gadget client: one implicit
session, in-memory chat (100 msgs), PTT voice, prompt dialog (chat screen
only), pairing display, 3 device actions it *answers*, battery-only
telemetry. The watch client (in flight) implements the **full** protocol
core — identity/HMAC/pairing, text, transcript/reply/status/notice,
prompt, display + image cards, audio sinks, state, OTA with progress.
Several verdicts therefore run in the watch's favour.

| # | Phone feature (what it does) | Verdict | Watch design / transport / cost / dependencies |
|---|---|---|---|
| H1 | **Chat bubble list** — USER/ASSISTANT/SYSTEM bubbles, auto-scroll, ★31dcd57 timestamps + 78% Muse-style bubbles | **ADAPT** | Watch view holds only the latest transcript+reply pair. Add a last-~6-exchange history ring (PSRAM, session-scoped) rendered as Hermes-dialect bubbles with timestamps. Plain text is parity — neither side renders markdown, and the hub pre-shapes text. Cost **M**. Dep: Hermes render work after UI Ph 3.7. |
| H2 | **Streaming replies** — `reply.delta` upserts the in-progress bubble; final `reply` settles it | **MATCHES** | Watch proto handles `reply.delta` cumulatively into the view already. No work. |
| H3 | **Interim replies as system lines** — non-final `reply` shown as a SYSTEM line, not spoken | **ADAPT** | Map interim replies onto the view's existing notice line instead of overwriting the reply buffer. Cost **S**. Dep: none. |
| H4 | **Stop generation** — Stop button sends `{type:"cancel"}` | **MATCHES** | Watch advertises the cancel input in caps and the adapter's top-press is CANCEL during a turn. No work (confirm in integration tests). |
| H5 | **New session** — dashboard button sends `session.new` + local system line | **PORT** | A real parity gap: the watch proto has no `session.new` send today. Add the frame + a trigger that needs no keyboard: long-press BOOT inside the Hermes app (and echo a local "new session" line into H1's history). Cost **S**. Dep: none. |
| H6 | **Hold-to-talk voice** — 20ms PCM16 frames, `audio.start/end`, <250ms → `audio.cancel` | **MATCHES** | Watch: hold bottom-center = TALK, release sends; proto implements start/end/cancel on ch1. No work. |
| H7 | **Spoken-reply playback** — server audio stream → jitter-buffered AudioTrack; new stream replaces old | **MATCHES** | Protocol core + audio sinks + `audio.start/end/abort` handling are in the watch client; the hub speaks to speaker-declaring devices and paces/resamples server-side. (Device speaker wiring was the client branch's stretch tier — verify at integration, but there is no *port* gap.) |
| H8 | **On-device TTS fallback** — Android TTS speaks the reply if the server sent no audio | **SKIP** | The fallback exists because a phone *can* go audioless; the hub always speaks to this watch (it declares a speaker). No TTS engine on the watch, and none is needed. |
| H9 | **Yes/No prompt dialog** — `prompt` → dialog → `prompt.reply`; `prompt.close` withdraws | **MATCHES** | Watch renders the prompt with YES/NO touch targets, TTL, single-answer semantics — and improves on the phone: HDB only surfaces prompts while its Chat screen is open, the watch is always on the wrist (see H10 for the not-foregrounded case). No work. |
| H10 | **Prompt surfacing when the app isn't in front** — (HDB gap: no prompt notification at all) | **ADAPT** | Route `prompt` (and `display` cards + `notice`) into the OS Notifications app via `add_notification()`; the Hermes link already pumps outside the foreground app, so the state is live — only the surfacing is missing. Prompts are TTL-bound: an unseen prompt is a lost answer. Cost **S**. Dep: UI Ph 3.5 notifications list. |
| H11 | **Pairing screen** — big code, paired state, host approve command shown | **MATCHES** | Watch view carries the 8-char code + the server's approve command; render the code hero-size per the tokens pairing panel (command string is host-side info — the code is the wrist artifact). No work. |
| H12 | **Pairing revoked handling** — `unpaired` → clear paired, back to PAIRING state | **ADAPT** | The contract defines `unpaired`; ensure the watch proto routes it to link-state Pairing + a notice (test-vector it in the host suite alongside the existing HMAC vectors). Cost **S**. Dep: none. |
| H13 | **Dashboard status** — connection dot, status text, live caption, paired label, ★31dcd57 battery % | **MATCHES** | The watch health line already renders BAT %/CHG + uptime + real LINK state, and the render shows status/notice text. No work. |
| H14 | **Telemetry** — `state {sensors:{battery_pct}}` in hello + every 60s | **MATCHES** | Watch proto sends `state` frames and advertises battery in caps/hello; cadence lives in the link layer. Battery is the only sensor on either side. No work. |
| H15 | **Device actions it answers** — `buddy.vibrate / buddy.notify / buddy.battery`, each answered exactly once | **ADAPT** (subset) | Watch proto already handles `action` → `action.result`. Advertise a watch set of exactly one: battery (the data exists; vibrate has **no motor on this board** — SKIP; notify is already covered by `display`/`notice` frames). Cost **S**. Dep: none. |
| H16 | **Image display** — phone: **STUB** (`image.start/end` parsed, CH_IMAGE frames explicitly dropped, "not implemented on this build") | **MATCHES — WATCH LEADS** | The watch client implements image cards (RGB565 chunks → framebuffer). The port direction reverses here: if anything, HDB should port this *from* the gadget firmware/watch client. No watch work. |
| H17 | **OTA updates** — phone: **STUB** (never advertised, never handled; updates are manual APK installs) | **MATCHES — WATCH LEADS** | Watch client implements the full contract flow (offer→ready→begin/HMAC→ch3 chunks→ack→verify→done + progress in the view) against the 2MB slots from the power pass. No watch work. |
| H18 | **Settings** — host URL, device name, mic rate; ★31dcd57 validation + auto-reconnect when host changes | **ADAPT** (display-only) | No keyboard: an info page showing host, device_id, paired state, firmware (all in client config). Editing stays Kconfig/NVS/host-side, exactly as the Muse side's M22. Mic rate is protocol-fixed at 16k on the watch — no setting needed. The ★ validation/auto-reconnect fixes are phone-side; the watch link already backs off and reconnects per contract. Cost **S**. Dep: none. |
| H19 | **Connection-error surfacing** — ★31dcd57 `onConnError` hint → lastError + notification text | **MATCHES** | Watch view already carries an error string (auth failures etc.) and the render/health line show real link states incl. AUTH FAIL. No work. |
| H20 | **Chat history persistence** — phone keeps 100 in memory, loses them on process death | **MATCHES** | Parity by design: both are volatile, session-scoped stores. H1's ring is the watch's equivalent of the 100-message flow. No work. |
| H21 | **Message long-press copy** — ★31dcd57 copy bubble text to clipboard | **SKIP** | No clipboard or text-selection idiom exists on the watch; there is nowhere to copy *to*. |
| H22 | **Avatar/character presence** — ★31dcd57 official Nous Girl art (static); animated stage is a STUB in the phone app | **ADAPT** | The canonical Hermes face is the gadget firmware's 1-bit mascot (blink/talk frames); DESIGN_TOKENS §9 rules it ART that stays 1-bit. Port the mascot bitmap set into the AIWatchOS Hermes screen (assets inside the existing FAT budget). Cost **S–M**. Dep: UI Ph 3.7 render. |
| H23 | **Device→server `event` messages** | **SKIP** | STUB in the phone app too (protocol type exists, never sent). Parity at zero; no watch need identified. |
| H24 | **Widgets / tiles / in-app update checks** | **SKIP** | None exist in the phone app. Watch updates are H17's real OTA, not a check-for-update screen. |

**Hermes totals: 24 features rated — PORT 1 · ADAPT 7 · MATCHES 12 · SKIP 4.**

Already-matched (do not rebuild): **H2 streaming, H4 cancel, H6 PTT,
H7 spoken replies, H9 prompts, H11 pairing screen, H13 status/health,
H14 telemetry, H19 error surfacing, H20 volatile history** — and the
watch outright **leads** the phone app on **H16 image display** and
**H17 OTA**, both of which are stubs in HermesDesktopBuddy's pushed main.

---

# Prioritized implementation lists

## Muse app — P0 (do these first; best value-to-cost on a watch)

1. **M2 — surface `agent.status`/`task.status`.** The events already
   arrive; the parser discards the payload. Parsing + the activityCaption
   map is the single cheapest change that makes the watch feel intelligent
   (drives the state word, ring, and THINKING visuals).
2. **M1 + M3 + M4 — the live, cleaned reply caption.** Stream the reply
   into the caption as it grows, cleaned by the ported caption function.
   This is the flagship surface (UI Ph 3.3) and the reason the app exists.
3. **M19 — avatar/state visuals as specced in Ph 3.3** (pixel stage, pose
   schemes, blink/breathing, pet tap). The companion's identity, at the
   size the budgets already approved.

## Muse app — P1

4. **M10 — cloud-TTS spoken replies** (gated on Duckets' TTS decision +
   key provisioning; the playback path is already vendored).
5. **M5 — heard-transcript bubble** (one parser event + one dim bubble).
6. **M22 — provisioning status page** (first-boot sanity; pairs with the
   flash checklist).
7. **M8 — display cards, if H-EVENT verifies** (render-only; cheapest
   card win — verify the host's chat-stream event vocabulary first).

## Muse app — P2

8. M6 mini history · M7 choice cards (needs H-EVENT + choice-return
   convention) · M9 reply-image on stage · M13 conversation mode ·
   M16 canned-prompt pills (needs text send) · M21 diagnostics page ·
   M27 notices → OS notifications (needs H-EVENT).

## Hermes app — P0 (the list is short because the client already matches the phone)

1. **H10 — prompts/cards/notices → OS Notifications.** The one real
   functional gap: Hermes prompts are TTL-bound and the watch's Hermes
   app is not always in front. Small cost, prevents silently lost answers.
2. **H5 — `session.new`.** One frame + a long-press trigger. The only
   conversation-management action the phone has that the watch lacks.

## Hermes app — P1

3. **H1 — bubble history ring** (last ~6 exchanges, timestamps,
   Hermes-dialect bubbles) — brings the watch's conversation surface to
   the phone's level.

## Hermes app — P2

4. H3 interim-as-notice · H12 `unpaired` routing + test vector ·
   H15 battery action advertisement · H18 info page ·
   H22 mascot bitmap port.

## Cross-cutting notes for whoever implements

- **H-EVENT is the Muse side's only real unknown.** Three ADAPTs (M7,
  M8, M27) hinge on whether the hatch host can emit card/notice events on
  `/chat/subscribe`. Verifying that vocabulary (one look at the host's
  chat-event emitter, or one instrumented session) converts three
  maybes into a sized work item — do it before any card UI is built.
- **One-TLS rule shapes M9/M10:** both must sequence their HTTPS calls
  around the persistent subscribe session and never overlap a turn
  upload/handshake (RESOURCE_BUDGET red flag 1).
- **Nothing in the P0/P1 lists spends SRAM** beyond the UI plan's
  existing ≤4KB envelope; histories and buffers are PSRAM rings with
  hard caps (M6: 4 exchanges; H1: 6 exchanges; reply text stays ≤4KB).
- Hermes P0/P1 deliberately excludes re-implementing H16/H17 — the
  watch already leads there; integration test time is better spent
  proving them on hardware.
