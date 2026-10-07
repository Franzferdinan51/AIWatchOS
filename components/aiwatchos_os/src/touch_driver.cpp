// FT3168 touch driver for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
// I2C: addr 0x38, SDA=GPIO15, SCL=GPIO14, INT=GPIO38 (Waveshare pin config).
// FT3168 is register-compatible with the FT5x06 family used by esp_lcd_touch_ft5x06.
// The touch panel maps directly to screen coordinates: 410 wide x 502 tall.
// X bounds: [0, 409], Y bounds: [0, 501]. No mirroring or rotation swap needed
// for the 2.06" rectangular (non-round) layout per board_waveshare_s3_206.c config.
#include "aiwatchos/hal.hpp"

namespace aiwatchos {

// Touch pin assignments from board_waveshare_s3_206.c / HermesGadget board.cpp.
constexpr int kTouchSda = 15;   // I2C data (Waveshare: GPIO 15 per the board file)
constexpr int kTouchScl = 14;   // I2C clock
constexpr int kTouchInt = 38;   // touch interrupt (active low, open drain)
constexpr int kTouchRst = 9;    // touch reset
constexpr uint8_t kTouchAddr = 0x38;

// FT3168 register addresses (same as FT5x06 family).
constexpr uint8_t kRegTouchPoints = 0x02;   // number of touch points
constexpr uint8_t kRegTouch1XHigh = 0x03;   // X high byte (bits 15:12 are flags)

// Read up to max_points touch coordinates. Returns the count read and fills
// out_xs/out_ys with screen-space values clamped to [0, width-1] x [0, height-1].
int ft3168_read(int* out_xs, int* out_ys, int max_points) {
    // The real implementation uses esp_lcd_touch_ft5x06 (register-compatible).
    // This stub returns zero points; the full driver is wired in display_start().
    return 0;
}

}  // namespace aiwatchos
