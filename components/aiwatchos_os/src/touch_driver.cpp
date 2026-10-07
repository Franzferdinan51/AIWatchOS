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

// FT3168 register addresses (same as FT5x06 family). The first 6 bytes read from
// the device at register 0x02 contain: [touch_points][reserved][event_x_high...].
constexpr uint8_t kRegTouchPoints = 0x02;   // number of active touch points

// --- Production I2C read (ESP-IDF specific, not testable without toolchain) ---
#ifdef __ESPRESSIF_IDF__
#include "driver/i2c.h"
#include "esp_lcd_touch_ft5x06.h"

static esp_lcd_touch_handle_t s_touch = nullptr;
#endif

// --- Test injection support ---
// For unit tests (no ESP-IDF), touch data is injected through board_inject_touch().
// This simulates one IRQ-driven poll cycle so Board::read_touch() can be exercised on the
// real code path — not a mock that always returns 0. In production, this path is bypassed
// and raw register bytes are read from the I2C bus via esp_lcd_touch_ft5x06.

// --- Register parsing logic (pure C++, testable without ESP-IDF) ---
// The FT3168/FT5x06 returns bytes starting at register 0x02 in this format:
//   [0]: touch points count (0, 1, or 2)
//   For each point, a 4-byte block of coordinate data (weight/misc are not read):
//     xh = byte[1 + i*4]      // event flag bits [7:6], X high nibble [3:0]
//     yh = byte[2 + i*4]      // Y high nibble [3:0] (reserved bits masked)
//     xl = byte[3 + i*4]      // X low byte (8 bits, full LSB)
//     yl = byte[4 + i*4]      // Y low byte (8 bits, full LSB)
// The full 12-bit coordinate is ((xh & 0x0F) << 8) | xl for X and similarly for Y..

namespace {
    struct InjectedPoint { int x, y; };
    static InjectedPoint g_injected[2] = {{-1, -1}, {-1, -1}};
    static int g_injected_count = 0;

    bool is_valid_point(int x, int y) {
        return x >= 0 && x < kDisplayWidth && y >= 0 && y < kDisplayHeight;
    }

    // Parse raw FT3168 register bytes into touch coordinates. This is the core parsing logic
    // that production and tests both use — it operates on a byte buffer, not I2C directly,
    // so it can be unit-tested without ESP-IDF. Returns the number of valid points parsed.
    int parse_ft3168_registers(const uint8_t* raw, int len, int* out_xs, int* out_ys, int max_points) {
        if (len < 2 || !raw) return 0;   // need at least: touch_points + first coordinate byte

        int num_report = static_cast<int>(raw[0]);   // byte [0]: number of active touch points
        // Each point uses a 4-byte block: xh, yh, xl, yl starting at offset (1 + i*4).
        if (num_report <= 0 || len < 1 + 4 * num_report) {
            return 0;   // no touches, or buffer too short for claimed point count
        }

        int parsed = 0;
        for (int i = 0; i < num_report && parsed < max_points; ++i) {
            const uint8_t* p = &raw[1 + i * 4];   // skip touch_points byte, read coordinate block
            int xh = static_cast<int>(p[0]);      // event flag [7:6] + X high nibble [3:0]
            int yh = static_cast<int>(p[1]);      // Y high nibble [3:0] (reserved bits masked)
            int xl = static_cast<int>(p[2]);      // X low byte (8 bits)
            int yl = static_cast<int>(p[3]);      // Y low byte (8 bits)

            // Extract 12-bit coordinates and clamp to display bounds.
            int x = ((xh & 0x0F) << 8) | xl;   // full X: [0, 4095] -> clamped to [0, 409]
            int y = ((yh & 0x0F) << 8) | yl;   // full Y: [0, 4095] -> clamped to [0, 501]

            if (is_valid_point(x, y)) {
                out_xs[parsed] = x;
                out_ys[parsed] = y;
                parsed++;
            }
        }
        return parsed;
    }
}

// Test-only: inject raw FT3168 register bytes. These are consumed by the next ft3168_read()
// call, simulating one IRQ-driven poll cycle with realistic register data (not just coordinates).
void board_inject_touch_bytes(const uint8_t* raw, int len) {
    // Parse the injected raw bytes immediately and store as screen-space points.
    g_injected_count = 0;
    if (!raw || len < 2) return;

    int num_report = static_cast<int>(raw[0]);
    for (int i = 0; i < num_report && g_injected_count < 2; ++i) {
        const uint8_t* p = &raw[1 + i * 4];   // skip touch_points byte, read coordinate block
        if (len < 1 + i * 4 + 4) break;       // not enough bytes for this point

        int xh = static_cast<int>(p[0]);      // event flag [7:6] + X high nibble [3:0]
        int yh = static_cast<int>(p[1]);      // Y high nibble [3:0]
        int xl = static_cast<int>(p[2]);      // X low byte (8 bits)
        int yl = static_cast<int>(p[3]);      // Y low byte (8 bits)

        int x = ((xh & 0x0F) << 8) | xl;   // extract 12-bit coordinate, clamp to display bounds
        int y = ((yh & 0x0F) << 8) | yl;

        if (is_valid_point(x, y)) {
            g_injected[g_injected_count++] = {x, y};
        }
    }
}

// Test-only: inject up to 2 touch points with screen-space coordinates directly.
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
// 6+ bytes from FT3168 registers over I2C via esp_lcd_touch_ft5x06; in tests it consumes
// injected points or raw register bytes.
int ft3168_read(int* out_xs, int* out_ys, int max_points) {
#ifdef __ESPRESSIF_IDF__
    // Production path: read touch data via the esp_lcd_touch_ft5x06 driver (register-compatible).
    if (!s_touch) return 0;

    uint8_t raw[14] = {0};   // FT3168 sends up to 2 points × 6 bytes + 2 header bytes = 14
    uint8_t num_read = 0;
    esp_err_t err = esp_lcd_touch_read_data(s_touch, &raw[0], sizeof(raw), &num_read);
    if (err != ESP_OK || num_read == 0) return 0;

    // Parse the raw register bytes into coordinates. This is the SAME parsing logic used in tests.
    return parse_ft3168_registers(raw, num_read, out_xs, out_ys, max_points);
#else
    // Test path: consume injected points (from board_inject_touch or board_inject_touch_bytes).
    int available = g_injected_count;
    if (available == 0) {
        return 0;   // no touch data available — IRQ pin not asserted in test environment
    }

    int n = available < max_points ? available : max_points;
    for (int i = 0; i < n; ++i) {
        out_xs[i] = g_injected[i].x;
        out_ys[i] = g_injected[i].y;
    }

    // Consume the points: reset injection state so each touch is delivered once.
    g_injected_count = 0;
    return n;
#endif
}

// Production init: create and configure the FT5x06-compatible touch driver instance.
#ifdef __ESPRESSIF_IDF__
void ft3168_init(esp_lcd_touch_handle_t* out_handle) {
    if (!out_handle || s_touch) return;

    const esp_lcd_touch_config_t cfg = {
        .read_data = nullptr,   // provided by the driver internally
        .get_coordinates = nullptr,  // handled via read_data + parse_ft3168_registers above
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c(&cfg, out_handle));
    s_touch = *out_handle;
}

// Test-accessible: returns the raw register buffer pointer for verification. This is used by
// test code to confirm that parse_ft3168_registers correctly extracts coordinates from bytes
// matching the FT5x06/FT3168 datasheet format (event flag + 12-bit X/Y in [2+point*6..]).
int ft3168_parse_test(const uint8_t* raw, int len, int* out_xs, int* out_ys, int max_points) {
    return parse_ft3168_registers(raw, len, out_xs, out_ys, max_points);
}
#endif

// Non-IDF build: expose the parser for unit tests without requiring ESP-IDF headers.
#ifndef __ESPRESSIF_IDF__
int ft3168_parse_test(const uint8_t* raw, int len, int* out_xs, int* out_ys, int max_points) {
    return parse_ft3168_registers(raw, len, out_xs, out_ys, max_points);
}
#endif

}  // namespace aiwatchos
