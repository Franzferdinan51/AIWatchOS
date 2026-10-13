# P1 port notes (2026-10-13)

Companion notes to FEATURE_PORT_MATRIX.md for the P1 wave (M10, M5,
M22, M8, H1). One matrix correction falls out of the wave: M8's
H-EVENT dependency resolves to NO — see below.

## M8 — BLOCKED-BY-SERVER (H-EVENT finding)

M8 (server display cards, render-only) was gated on H-EVENT: does the
hatch host emit any display/card event on `/chat/subscribe`? Two
independent witnesses say the subscribe vocabulary has no such event:

1. **The reference gadget firmware** (`muse-gadget-206`,
   `esp32/components/muse/muse_chat_link.c`, `subscription_event()`):
   the complete handled vocabulary is `delta.message_start`,
   `delta.text_append`, `delta.message_done`, `message.assistant`,
   `message.user`. (The watch additionally handles `agent.status` /
   `task.status`, which the companion consumes.) The only "card" in
   the firmware is `card_ttl`, a FreeRTOS timer name in
   `muse_status.c` for its LOCAL status card — not a wire event.
2. **The companion** (`MuseCompanionApp/app/lib/app/chat.dart`,
   `applyServerEvent()` — the complete fold of `/chat/subscribe`
   events into chat history): user-role / `transcript` /
   `message.user`, `agent.status` / `task.status`,
   `delta.message_start`, `delta.text_append`,
   `delta.message_done` / `message.assistant`. Nothing else.

Cards DO exist in the companion, but on a different channel:
`choice_cards.dart` — the agent shows a card by calling the Link
invoke `display.show_card` on the phone, and reads the result back
with `display.card_status`. That is the Link invoke surface, which
the watch does not speak (the matrix's M7 row says the same).

**Conclusion:** no display/card event is emitted on the chat
subscribe stream by any proven client contract, so there is nothing
faithful to render. M8 is BLOCKED-BY-SERVER; no card UI was built.
M7 (choice cards) and M27 (server notices) inherit the same block:
both need a host-side event that does not exist today. If the host
ever emits one, the Hermes client's display-card renderer is the
in-tree pattern to mirror, and M8 becomes the S–M port the matrix
estimated.

## Contracts the P1 wave built against

- **M5** (`message.user`): field order and relation rules from the
  gadget firmware's `muse_chat_link.c` (event's own `message_id`
  against the chat-ack ids; `display_text_ready` gate; text =
  `transcript` ?? `display_text` ?? `content` ?? `text` per the
  companion's `applyServerEvent`; `\n[file:` attachment suffix cut
  and the `[Voice note]` placeholder suppressed, per the firmware's
  `on_row`).
- **M10** (cloud TTS): request shape from the companion's
  `openrouter_tts.dart` — POST an OpenAI-shaped `/v1/audio/speech`
  with `{"model","input","response_format":"mp3"}` (+ `voice` when
  set), `Authorization: Bearer <key>`, text chunked at 1800 chars on
  sentence boundaries, MP3 response body, 60 s timeout. The watch
  generalizes the endpoint/model/voice/key to provisioning (the
  companion's own defaults are the Kconfig defaults) and adds one
  guard the companion lacks: a single sentence over the cap is
  hard-split at the cap so no request ever exceeds it.
- **H1 / M22 / M5 rendering**: no server contract involved — watch
  UI over state the clients already hold (Hermes view pair;
  net_config presence booleans).
