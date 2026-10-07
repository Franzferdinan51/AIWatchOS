// ES8311 (speaker) + ES7210 (mic) codec driver via I2S for the 2.06" board.
// Pin map from the upstream muse-gadget-206 board_waveshare_s3_206.c:
//   MCLK=GPIO16, BCLK=GPIO41, WS=GPIO45, DOUT=GPIO42 (speaker), DIN=GPIO40 (mic)
//   PA enable = GPIO46
// Both codecs share one I2S duplex bus (16 kHz stereo 16-bit) and the shared
// I2C bus (ES8311 addr 0x18, ES7210 addr 0x10). Bring-up mirrors the upstream
// audio_init(), adapted to the esp_codec_dev 1.6 API (codec_if + data_if
// handles instead of the older single-call constructors).
#include "aiwatchos/hal.hpp"

#ifdef __ESPRESSIF_IDF__
#include "audio_codec_gpio_if.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#endif

namespace aiwatchos {

constexpr int kAudioMclk = 16;
constexpr int kAudioBclk = 41;
constexpr int kAudioWs   = 45;
constexpr int kAudioDout = 42;   // to ES8311 (speaker)
constexpr int kAudioDin  = 40;    // from ES7210 (mics)
constexpr int kAudioPa   = 46;    // power amp enable
constexpr uint32_t kAudioRateHz = 16000;   // voice pipeline rate (matches upstream)

#ifdef __ESPRESSIF_IDF__
namespace {
const char* kAudioTag = "audio";
i2s_chan_handle_t s_tx = nullptr;
i2s_chan_handle_t s_rx = nullptr;
esp_codec_dev_handle_t s_spk = nullptr;
esp_codec_dev_handle_t s_mic = nullptr;
bool s_playing = false;
bool s_capturing = false;
}  // namespace

// Bring up I2S, both codecs and their codec-device handles. Called once from
// Board::begin(). Returns ESP_OK when playback and capture are usable.
esp_err_t audio_codecs_init(void) {
    if (s_spk && s_mic) return ESP_OK;

    // Speaker power amp on.
    gpio_config_t pa = {};
    pa.pin_bit_mask = 1ULL << kAudioPa;
    pa.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&pa), kAudioTag, "PA enable gpio");
    gpio_set_level(static_cast<gpio_num_t>(kAudioPa), 1);

    // I2S standard mode, 16 kHz stereo 16-bit, ESP32-S3 as clock master.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), kAudioTag, "I2S channels");
    i2c_master_bus_handle_t bus = aiwatchos_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;
    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kAudioRateHz);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                           I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = static_cast<gpio_num_t>(kAudioMclk);
    std_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(kAudioBclk);
    std_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(kAudioWs);
    std_cfg.gpio_cfg.dout = static_cast<gpio_num_t>(kAudioDout);
    std_cfg.gpio_cfg.din = static_cast<gpio_num_t>(kAudioDin);
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), kAudioTag, "I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), kAudioTag, "I2S RX");

    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();
    if (!gpio_if) return ESP_ERR_NO_MEM;

    // ES8311 speaker on the shared I2C bus. NOTE: this framework version
    // (1.6.x audio_codec_ctrl_i2c.c) shifts the address right by one, so it
    // takes 8-bit form: 0x30 (== 7-bit 0x18, ES8311_CODEC_DEFAULT_ADDR).
    audio_codec_i2c_cfg_t spk_i2c = {};
    spk_i2c.port = I2C_NUM_0;
    spk_i2c.addr = 0x30;
    spk_i2c.bus_handle = bus;
    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = audio_codec_new_i2c_ctrl(&spk_i2c);
    es8311_cfg.gpio_if = gpio_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = kAudioPa;
    es8311_cfg.master_mode = false;
    es8311_cfg.use_mclk = true;
    if (!es8311_cfg.ctrl_if) return ESP_ERR_NO_MEM;
    const audio_codec_if_t* spk_codec_if = es8311_codec_new(&es8311_cfg);
    if (!spk_codec_if) return ESP_ERR_NO_MEM;

    audio_codec_i2s_cfg_t spk_i2s = {};
    spk_i2s.port = I2S_NUM_0;
    spk_i2s.rx_handle = s_rx;
    spk_i2s.tx_handle = s_tx;
    esp_codec_dev_cfg_t spk_dev = {};
    spk_dev.codec_if = spk_codec_if;
    spk_dev.data_if = audio_codec_new_i2s_data(&spk_i2s);
    if (!spk_dev.data_if) return ESP_ERR_NO_MEM;
    s_spk = esp_codec_dev_new(&spk_dev);
    if (!s_spk) return ESP_ERR_NO_MEM;
    esp_codec_dev_sample_info_t spk_fs = {};
    spk_fs.sample_rate = kAudioRateHz;
    spk_fs.channel = 2;
    spk_fs.bits_per_sample = 16;
    if (esp_codec_dev_open(s_spk, &spk_fs)) return ESP_FAIL;

    // ES7210 dual-mic ADC on the shared I2C bus. Same 8-bit convention:
    // 0x80 (== 7-bit 0x40, ES7210_CODEC_DEFAULT_ADDR).
    audio_codec_i2c_cfg_t mic_i2c = {};
    mic_i2c.port = I2C_NUM_0;
    mic_i2c.addr = 0x80;
    mic_i2c.bus_handle = bus;
    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = audio_codec_new_i2c_ctrl(&mic_i2c);
    es7210_cfg.master_mode = false;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2;
    if (!es7210_cfg.ctrl_if) return ESP_ERR_NO_MEM;
    const audio_codec_if_t* mic_codec_if = es7210_codec_new(&es7210_cfg);
    if (!mic_codec_if) return ESP_ERR_NO_MEM;

    audio_codec_i2s_cfg_t mic_i2s = {};
    mic_i2s.port = I2S_NUM_0;
    mic_i2s.rx_handle = s_rx;
    mic_i2s.tx_handle = s_tx;
    esp_codec_dev_cfg_t mic_dev = {};
    mic_dev.codec_if = mic_codec_if;
    mic_dev.data_if = audio_codec_new_i2s_data(&mic_i2s);
    if (!mic_dev.data_if) return ESP_ERR_NO_MEM;
    s_mic = esp_codec_dev_new(&mic_dev);
    if (!s_mic) return ESP_ERR_NO_MEM;
    esp_codec_dev_sample_info_t mic_fs = {};
    mic_fs.sample_rate = kAudioRateHz;
    mic_fs.channel = 2;
    mic_fs.bits_per_sample = 16;
    if (esp_codec_dev_open(s_mic, &mic_fs)) return ESP_FAIL;
    esp_codec_dev_set_in_gain(s_mic, 30.0);   // ES7210 PGA ~30 dB (matches upstream)
    return ESP_OK;
}

// The Hal moves mono frames; the hardware runs stereo. Playback duplicates
// mono to both channels; capture averages stereo down to mono.
bool aiwatchos_audio_start_playback(uint32_t rate) {
    if (!s_tx || rate != kAudioRateHz) return false;
    if (s_playing) return true;
    if (i2s_channel_enable(s_tx) != ESP_OK) return false;
    s_playing = true;
    return true;
}

void aiwatchos_audio_write(const int16_t* samples, size_t frames) {
    if (!s_playing || !samples) return;
    int16_t stereo[512];
    size_t done = 0;
    while (done < frames) {
        size_t chunk = frames - done > 256 ? 256 : frames - done;
        for (size_t i = 0; i < chunk; ++i) {
            stereo[2 * i] = samples[done + i];
            stereo[2 * i + 1] = samples[done + i];
        }
        size_t written = 0;
        if (i2s_channel_write(s_tx, stereo, chunk * 4, &written, 20) != ESP_OK) return;
        done += written / 4;
        if (written == 0) return;
    }
}

void aiwatchos_audio_stop(void) {
    if (!s_playing) return;
    i2s_channel_disable(s_tx);
    s_playing = false;
}

void aiwatchos_audio_set_volume(uint8_t percent_0_to_100) {
    if (!s_spk) return;
    if (percent_0_to_100 > 100) percent_0_to_100 = 100;
    esp_codec_dev_set_out_vol(s_spk, static_cast<int>(percent_0_to_100));
}

bool aiwatchos_mic_start(uint32_t rate) {
    if (!s_rx || rate != kAudioRateHz) return false;
    if (s_capturing) return true;
    if (i2s_channel_enable(s_rx) != ESP_OK) return false;
    s_capturing = true;
    return true;
}

size_t aiwatchos_mic_read(int16_t* out, size_t frames) {
    if (!s_capturing || !out || frames == 0) return 0;
    int16_t stereo[512];
    size_t got = 0;
    while (got < frames) {
        size_t chunk = frames - got > 256 ? 256 : frames - got;
        size_t bytes = 0;
        if (i2s_channel_read(s_rx, stereo, chunk * 4, &bytes, 20) != ESP_OK) break;
        size_t stereo_frames = bytes / 4;
        for (size_t i = 0; i < stereo_frames; ++i) {
            out[got + i] = static_cast<int16_t>((stereo[2 * i] + stereo[2 * i + 1]) / 2);
        }
        got += stereo_frames;
        if (bytes == 0) break;
    }
    return got;
}

void aiwatchos_mic_stop(void) {
    if (!s_capturing) return;
    i2s_channel_disable(s_rx);
    s_capturing = false;
}
#endif  // __ESPRESSIF_IDF__

}  // namespace aiwatchos
