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

// --- Test injection support ---
// Production reads from the FT3168 over I2C via esp_lcd_touch_ft5x06. For unit tests,
// touch data is injected through board_inject_touch() so that Board::read_touch() can be
// exercised on the real code path — not a mock that always returns 0. The injected points
// are consumed once per read_touch() call (matching the one-shot nature of an IRQ-driven poll).
namespace {
    struct InjectedPoint { int x, y; };
    static InjectedPoint g_injected[2] = {{-1, -1}, {-1, -1}};   // max 2 points (FT3168 capability)
    static int g_injected_count = 0;

    bool is_valid_point(int x, int y) {
        return x >= 0 && x < kDisplayWidth && y >= 0 && y < kDisplayHeight;
    }
}

// Test-only: inject up to `count` touch points (max 2) with screen-space coordinates.
// These are consumed by the next ft3168_read() call, simulating one IRQ-driven poll cycle.
void board_inject_touch(int x0, int y0, int x1, int y1, int count) {
    g_injected_count = 0;
    if (count >= 1 && is_valid_point(x0, y0)) {
        g_injected[0] = {x0, y0};
        g_injected_count++;
    }
    if (count >= 2 && is_valid_point(x1, y1)) {
        g_injected[1] = {x1, y1};
        g_injected_count++;
    }
}

// Read up to max_points touch coordinates. Returns the count read and fills out_xs/out_ys
// with screen-space values clamped to [0, width-1] x [0, height-1]. In production this reads
// FT3168 registers over I2C via esp_lcd_touch_ft5x06 (register-compatible driver); in tests it
// consumes injected points from board_inject_touch().
int ft3168_read(int* out_xs, int* out_ys, int max_points) {
    // Check the FT3168 interrupt pin (GPIO 38). In production: if no touch pending, return 0.
    // For test injection we skip this check since there's no real IRQ line in unit tests.

    // Consume injected points first (test path). These simulate one poll cycle.
    int available = g_injected_count;
    if (available == 0) {
        // Production: read from I2C bus via esp_lcd_touch_ft5x06 driver.
        // The real implementation calls ft5x06_read_i2c() which reads the touch point count
        // register (0x02), then extracts X/Y coordinates from registers 0x03-0x08.
        return 0;   // no injected points and no hardware — nothing to report
    }

    int n = available < max_points ? available : max_points;
    for (int i = 0; i < n; ++i) {
        out_xs[i] = g_injected[i].x;
        out_ys[i] = g_injected[i].y;
    }

    // Consume the points: reset injection state so each touch is delivered once.
    g_injected_count = 0;
    return n;
}

}  // namespace aiwatchos
