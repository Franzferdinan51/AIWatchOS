# Spoken replies (TTS) — implemented design

Status: **implemented** on branch `tts-port` (2026-10-13). This is the
port `docs/tts-investigation.md` specified: the investigation's verdict
was that no speech source exists behind the Hatch token and the
gadget's MP3 machinery is dormant, so AIWatchOS synthesizes speech the
same way the companion app does — by calling an external speech API
with a user-supplied key — and plays it through the MP3 path that was
already built and host-tested (`muse_play_reply`).

## Flow

1. A Muse turn completes on the link task (`muse_link.cpp`,
   `upload_turn_impl`): the reply text is published to
   `muse_link_last_reply()` as before, then handed to
   `tts_speak_reply()` (`components/aiwatchos_net/src/muse_tts.cpp`).
2. Gate: if no TTS API key is configured, or no speech sink is
   registered, the function returns immediately — **no request is ever
   attempted** and the reply stays text. With the default (empty) key
   the firmware's behavior is byte-for-byte the pre-port behavior.
3. The reply is cleaned with the caption cleaner's rules
   (`proto::speech_clean` — a mirror of
   `apps/muse/include/muse_captions.hpp` `clean()`: fenced code →
   one space, markdown links → their text, markdown chars removed,
   whitespace collapsed) but **without** the caption's 240-char clip.
4. The cleaned text is chunked (policy below) and each chunk is
   fetched and played **sequentially**: POST → stream MP3 → play →
   next chunk. Playback runs through the speech sink the Muse app
   registered in `muse_init` (`muse_link_set_speech_sink` →
   `muse_play_reply`). Everything runs on the link task; the UI loop
   never blocks on a fetch.
5. While a clip is actually playing, `muse_link_speaking()` is true
   and `muse_face_state()` returns SPEAKING (checked right after
   LISTENING, ahead of the outbox/agent-status inference). When the
   clip ends the signal drops and the face returns to its prior state.

## Provider contract

Any OpenAI-shaped speech endpoint works; nothing is baked in as
mandatory:

```
POST {base_url}/v1/audio/speech
Authorization: Bearer {api_key}
Content-Type: application/json

{"model": "...", "voice": "...", "input": "<chunk>",
 "response_format": "mp3"}
```

The response body is the MP3. This is the contract OpenRouter, OpenAI
and compatible/self-hosted servers share. `base_url` is the **full API
prefix** (the watch appends `/v1/audio/speech`), so e.g.
`https://api.openai.com` and `https://openrouter.ai/api` both fit.

## Configuration

Same seam as all other aiwnet config: Kconfig defaults → NVS
(namespace `aiwnet`) overrides → `aiwatchos://setup?...` provisioning
string (keys `ttsurl`, `ttskey`, `ttsmodel`, `ttsvoice`). The key is
never logged.

| Field | Kconfig | NVS key | Default |
|---|---|---|---|
| Base URL | `AIWATCHOS_NET_TTS_BASE_URL` | `ttsurl` | `https://openrouter.ai/api` |
| API key | `AIWATCHOS_NET_TTS_API_KEY` | `ttskey` | `""` — **empty = spoken replies OFF** |
| Model | `AIWATCHOS_NET_TTS_MODEL` | `ttsmodel` | `openai/gpt-4o-mini-tts` |
| Voice | `AIWATCHOS_NET_TTS_VOICE` | `ttsvoice` | `alloy` |

There is no on-watch settings UI for these yet (same as the other
network credentials); provisioning is Kconfig or the setup string.

## Chunking policy (as implemented)

In `proto::speech_chunks` (`net_proto.cpp`, host-tested):

- Input is the **cleaned** full reply (never the 240-char caption).
- At most **1000 code points** per request. Caps count UTF-8 code
  points; a cut never splits a code point.
- Split preference within the 1000-code-point window: the **last
  sentence ender** (`.`, `!`, `?`, `…`) that is followed by a space or
  by the window's end — so `3.14` never splits mid-number — else the
  **last space**, else a **hard cut** at the cap.
- At most **3 chunks** per reply (~3 minutes of speech at typical
  rates). Any remainder is **dropped to text**: it is not fetched,
  not spoken — and the caption still shows the complete reply, so
  nothing is lost, just not read aloud.
- An empty cleaned reply (e.g. a reply that was only a code block)
  speaks nothing.

## Failure semantics (fail-closed)

Speech must never break the text path. Each of these drops the reply
(or its remaining chunks) to text, silently apart from a log line:

- TTS unconfigured / no sink → no request attempted.
- HTTP error, non-200 status, TLS failure, 30 s fetch timeout.
- Response over the **512 KB cap** (`proto::kSpeechMp3CapBytes`, the
  reference gadget's MP3_BUF scale): the fetch aborts — the clip is
  never truncated into a half-playable file.
- PSRAM exhaustion while streaming (the buffer is PSRAM-only,
  doubling from 64 KB; internal DRAM is never used for it).
- Playback refusal (`muse_play_reply` returns 0 frames).

The pipeline state machine (`proto::SpeechMachine`:
idle → fetching → playing → fetching … → idle, any failure → failed)
is pure and host-tested; `muse_tts.cpp` drives it.

One honest cost: while a long reply is fetched + played, the link
task is busy speaking, so the next outbox pump (and the next turn's
upload) waits. Fetches are bounded by the 30 s timeout; playback is
bounded by the chunk budget.

## Tests

`components/aiwatchos_net/test/net_logic_test.cpp` covers: the
cleaner (vectors shared with the caption tests), the chunker
(sentence boundaries, decimal points, word fallback, no-boundary hard
cuts, exact caps, UTF-8 integrity, chunk budget), URL joining, the
JSON body (shape + escaping), the setup-string config fields, and the
state machine transitions (idle→fetching→playing→idle,
failure→failed→recovery). Request gating by the empty key is enforced
in `tts_speak_reply` before any fetch; the response-cap logic is the
device fetch sink, structured so the cap check precedes every write.
All pre-existing suites still pass (50 root, net logic, weather
logic).

## Size gate (ESP-IDF v5.3.6, A/B vs pristine 9570ebe)

- Image: 1,296,384 B vs baseline 1,265,488 B; 38% of the 2 MB slot
  free (gate ≥ 35%).
- Static DIRAM: 123,483 B vs baseline 116,451 B (+7,032).
  - +6,912 B: `muse_play_reply`'s pre-existing decode staging
    statics (4,608 B frame + 2,304 B resample buffer). They existed
    in the shipped source all along; the baseline linked them out
    because nothing called the function. Decode inner-loop working
    set; DIRAM remains 64% free.
  - +117 B: this port's net-component globals (speech machine, sink
    pointers, config strings' object storage).
  - Flash +19,682 B is the vendored minimp3 decoder linking for the
    first time, for the same reason; +7,591 B is the speech logic
    and fetch glue.
- Every buffer this port allocates (the ≤ 512 KB MP3 stream) is
  PSRAM-only and fail-closed.
