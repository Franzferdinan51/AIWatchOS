// ES8311 (speaker) + ES7210 (mic) codec driver via I2S for the 2.06" board.
// Pin map from board_waveshare_s3_206.c:
//   MCLK=GPIO16, BCLK=GPIO41, WS=GPIO45, DOUT=GPIO42 (speaker), DIN=GPIO40 (mic)
//   PA enable = GPIO46
// Both codecs share one I2S duplex bus and the same I2C bus (addr 0x18 for ES8311,
// 0x10/0x11 for ES7210). Sample rate: 16 kHz PCM16 mono.
#include "aiwatchos/hal.hpp"

namespace aiwatchos {

constexpr int kAudioMclk = 16;
constexpr int kAudioBclk = 41;
constexpr int kAudioWs   = 45;
constexpr int kAudioDout = 42;   // to ES8311 (speaker)
constexpr int kAudioDin  = 40;    // from ES7210 (mics)
constexpr int kAudioPa   = 46;    // power amp enable

}  // namespace aiwatchos
