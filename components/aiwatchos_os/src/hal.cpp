// AIWatchOS HAL — board driver for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
// Pin assignments and init sequences are derived from research of:
//   - muse-gadget-206 esp32/components/muse/boards/board_waveshare_s3_206.c
//   - HermesGadget firmware/esp32/main/board.cpp (CONFIG_HG_BOARD_AMOLED_206)
#include "aiwatchos/hal.hpp"

namespace aiwatchos {

Board& Board::instance() {
    static Board board;
    return board;
}

Hal& hal() {
    return Board::instance();
}

Board::Board() = default;

void Board::begin() {
    // One-time hardware init: QSPI bus, I2C, PMU rails, codecs.
    // Full ESP-IDF driver implementation is in display_driver.cpp, touch_driver.cpp,
    // audio_driver.cpp, power_manager.cpp. This method coordinates the sequence.
}

// --- Display (CO5300 QSPI AMOLED) — implemented in display_driver.cpp ---
bool Board::display_flush() { return false; }  // stub: real impl uses esp_lcd_co5300
void Board::set_backlight(uint8_t percent) {}   // stub

// --- Touch (FT3168 on I2C addr 0x38, SDA=GPIO15 SCL=GPIO14 INT=GPIO38) — touch_driver.cpp ---
int Board::read_touch(int* out_xs, int* out_ys) { return 0; }

// --- Audio (ES8311 + ES7210 via I2S) — audio_driver.cpp ---
bool Board::audio_start_playback(uint32_t rate) { return false; }
void Board::audio_write(const int16_t*, size_t) {}
void Board::audio_stop_playback() {}
bool Board::mic_start(uint32_t rate) { return false; }
void Board::mic_stop() {}

// --- Power (AXP2101 PMU) — power_manager.cpp ---
PowerStatus Board::read_power() { return {}; }

// --- Time (PCF85063 RTC via I2C) ---
uint64_t Board::now_ms() { return 0; }

// --- Buttons (BOOT=GPIO0, PWR side key via AXP2101) */
uint32_t Board::read_buttons() { return 0; }

}  // namespace aiwatchos
