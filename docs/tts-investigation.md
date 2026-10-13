# Spoken replies (TTS) — investigation, 2026-10-13

> **Status update (2026-10-13, later): IMPLEMENTED.** The port this
> investigation specified was built the same day on the `tts-port`
> branch, provider-agnostic as recommended here: an OpenAI-shaped
> speech endpoint configured through the aiwnet Kconfig/NVS seam
> (base URL default `https://openrouter.ai/api`, key default EMPTY =
> spoken replies off). The implemented design — flow, config,
> chunking policy, failure semantics, size gate — lives in
> `docs/tts.md`. The investigation below stands as the trace and
> rationale.

**Question:** Hatch replies arrive as text. Can the watch speak them, the way
muse-gadget-206 supposedly does — using only the credentials the watch
already has (the Hatch device/VM token)?

**Answer: No — because muse-gadget-206 does not speak replies either.** There
is no audio in the Hatch chat protocol, and no TTS endpoint behind the Hatch
token. Spoken replies in this ecosystem come from a *separate* TTS API with
its *own* key (the companion app uses OpenRouter). Closing the gap needs a
provider choice + API key from the owner; the watch-side integration is then
small and fully specified below.

## The trace (muse-gadget-206, ~/proj/muse-gadget-206, main 86596a5)

All citations are esp32/components/muse/muse_chat_session.cpp unless noted.

1. **The protocol carries text only.** A turn is: POST /api/voice/dictation
   (mic up, transcripts back), POST /chat/stream, and replies as events on
   the long-lived POST /chat/subscribe NDJSON stream — delta.message_start /
   text_append / message_done (file header, lines 22–36). The subscribe
   event parser dispatches text events; there is no audio event type.
2. **The gadget paces captions with silence; it never fetches speech.**
   start_tts() (line 1495) computes a *silence* duration from the message
   length (m.pcm_frames = m.len * MIC_RATE / TEXT_CHARS_PER_S), sets
   s_turn.silent = true, and pace_silently() (line 1558) feeds zeroed PCM
   into the reply buffer. Its own header says it plainly (lines 29–30):
   "see start_tts to speak it with a TTS API of your own; Muse doesn't
   speak gadget replies."
3. **The MP3 machinery is dormant plumbing, not a source.** The stream-kind
   enum has K_TTS (line 175) and tts_data()/tts_end()/decode() buffer and
   decode MP3 (up to MP3_BUF = 512 KB, line 95) — but open_stream() is only
   ever called for K_SUB, K_DICT and K_CHAT (lines 804, 1030, 1116, 1180),
   **never K_TTS**, so nothing ever delivers bytes to tts_data(). The
   diagnostics marks table (MARK_NAMES, line 254) contains "tts" / "mp3" /
   "audio", but mark(M_TTS) is never emitted and mark(M_MP3) fires only
   inside the unreachable tts_data().
4. **The gadget documents the intended integration itself** (comment block
   in start_tts(), lines 1506–1521): send the finished message text *"to a
   TTS API of your choice and play the MP3 it returns"*, keeping the message
   TTS_ACTIVE, feeding MP3 to tts_data() as it arrives and setting
   mp3_ended at the end. I.e. the speech source is an external TTS API
   chosen — and credentialed — by whoever integrates it.
5. **No TTS hides behind the Hatch credentials.** The account API
   (muse_account_api.h) does exactly one thing: find the VM a device token
   can reach and its per-VM Noise credential. The gadget advertises no
   speech/tts capability in its registration JSON, and no request it makes
   asks the VM for audio.
6. **The companion app confirms the pattern.** MuseCompanionApp speaks
   replies two ways: native phone TTS, or cloud TTS via **OpenRouter**
   (app/lib/app/openrouter_tts.dart — OpenAI-shaped /v1/audio/speech) using
   an **"OpenRouter API key for cloud TTS, kept in encrypted storage"**
   (app/lib/app/storage.dart, line 86). A separate service, a separate key,
   stored on the phone — nothing the watch holds or can borrow.

(Earlier shorthand that the gadget "synthesizes speech via a TTS service"
was wrong in the way that matters: it *can* be wired to one; as shipped it
speaks nothing. AIWatchOS's own muse_link header — components/aiwatchos_net/
src/muse_link.cpp, "Replies" note — already records this correctly.)

## What AIWatchOS already has ready

- muse_play_reply(mp3, len) — MP3 decode + playback path, implemented and
  host-tested (apps/muse/include/aiwatchos_muse.hpp line 73;
  apps/muse/src/muse_adapter.cpp line 708). Nothing feeds it.
- The Muse face has a SPEAKING state/scheme; today it can only be inferred
  from agent status codes. Real playback start/stop is the honest signal to
  drive it, and the adapter seam for that exists.

## What is needed to finish this (owner decision, then a small port)

1. **Duckets picks a TTS provider and supplies an API key.** The in-family
   choice is OpenRouter (same service + key shape the companion already
   uses; OpenAI-compatible speech endpoint returning MP3). Any
   OpenAI-shaped /v1/audio/speech provider works the same.
2. Port, following the gadget's own recipe: in muse_link, when a reply
   message finishes, POST its (cleaned, clipped) text to the provider from
   the link task over the existing TLS plumbing; stream the MP3 into a
   PSRAM buffer capped at the gadget's MP3_BUF scale (512 KB, fail-closed,
   drop-and-stay-text on overflow/error); hand it to muse_play_reply;
   hold the Muse face in SPEAKING for the playback duration. Text replies
   remain the fallback on any failure — speech must never block or break
   the caption path. Config: provider base URL + key via the existing
   Kconfig/NVS setup seam, empty = speech off.
3. Host tests mirroring the gadget's shape: TTS fetch state transitions,
   buffer-cap enforcement, failure-to-text fallbacks.

No code was changed by this investigation: implementing against an
endpoint that does not exist for the watch's credentials would be fiction.
