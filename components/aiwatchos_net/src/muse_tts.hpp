// Spoken replies (TTS) — device glue internal header. The public
// surface is net.hpp (muse_link_set_speech_sink / muse_link_speaking);
// the design and the trace that led here live in
// docs/tts-investigation.md + docs/tts.md.
#pragma once

#include <string>

namespace aiwatchos_net {

// Speaks one completed reply: cleans it, chunks it, fetches each
// chunk's MP3 from the configured speech endpoint and hands it to the
// registered speech sink for playback. Called on the link task at
// reply completion (muse_link.cpp). Returns true when the whole reply
// was spoken. Any failure — unconfigured, no sink, HTTP error,
// oversize response, playback refusal — returns false and leaves the
// text reply standing; speech never breaks the text path.
bool tts_speak_reply(const std::string& reply_text);

}  // namespace aiwatchos_net
