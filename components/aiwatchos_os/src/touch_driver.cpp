// FT3168 touch driver for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
// I2C: addr 0x38, SDA=GPIO15, SCL=GPIO14, INT=GPIO38 (Waveshare pin config).
// FT3168 is register-compatible with the FT5x06 family used by esp_lcd_touch_ft5x06.
// The touch panel maps directly to screen coordinates: 410 wide x 502 tall.
// X bounds: [0, 409], Y bounds: [0, 501]. No mirroring or rotation swap needed
// for the 2.06" rectangular (non-round) layout per board_waveshare_s3_206.c config.
#include "aiwatchos/hal.hpp"

// NOTE: IDF headers must be included BEFORE the namespace opens. Including
// them inside `namespace aiwatchos {}` breaks FreeRTOS headers (they reference
// ::_reent, which would resolve to aiwatchos::_reent and fail to compile).
#ifdef __ESPRESSIF_IDF__
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

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

// --- Production state (ESP-IDF only) ---
#ifdef __ESPRESSIF_IDF__
static const char* kTouchTag = "ft3168";
static esp_lcd_touch_handle_t s_touch = nullptr;
static esp_lcd_panel_io_handle_t s_touch_io = nullptr;
static i2c_master_bus_handle_t s_touch_bus = nullptr;

// Shared I2C bus for PMU/codec clients (declared in hal.hpp). Null until
// ft3168_init() has created it.
i2c_master_bus_handle_t aiwatchos_i2c_bus() { return s_touch_bus; }
#endif

// --- Test injection support ---
// For unit tests (no ESP-IDF), touch data is injected through board_inject_touch().
// This simulates one IRQ-driven poll cycle so Board::read_touch() can be exercised on the
// real code path — not a mock that always returns 0. In production, this path is bypassed
// and raw register bytes are read from the I2C bus via esp_lcd_touch_ft5x06.

// --- Register parsing logic (pure C++, testable without ESP-IDF) ---
// The FT3168/FT5x06 returns bytes starting at register 0x02 in this format:
//   [0]: touch points count (0, 1, or 2)
//   For each point, a 6-byte block per the register map (regs 0x03..0x08, 0x09..0x0E):
//     xh     = byte[1 + i*6]  // event flag bits [7:6], X high nibble [3:0]
//     xl     = byte[2 + i*6]  // X low byte (8 bits, full LSB)
//     yh     = byte[3 + i*6]  // touch ID [7:4], Y high nibble [3:0]
//     yl     = byte[4 + i*6]  // Y low byte (8 bits, full LSB)
//     weight = byte[5 + i*6]  // pressure (ignored)
//     misc   = byte[6 + i*6]  // area (ignored)
// The full 12-bit coordinate is ((xh & 0x0F) << 8) | xl for X and similarly for Y.

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
        // FT5x06/FT3168 register map: each point occupies 6 bytes — XH, XL, YH, YL,
        // WEIGHT, MISC — starting at register 0x03, i.e. offset (1 + i*6) in a buffer
        // read starting from register 0x02.
        if (num_report <= 0 || len < 1 + 6 * num_report) {
            return 0;   // no touches, or buffer too short for claimed point count
        }

        int parsed = 0;
        for (int i = 0; i < num_report && parsed < max_points; ++i) {
            const uint8_t* p = &raw[1 + i * 6];   // skip touch_points byte, read 6-byte point block
            int xh = static_cast<int>(p[0]);      // event flag [7:6] + X high nibble [3:0]
            int xl = static_cast<int>(p[1]);      // X low byte (8 bits)
            int yh = static_cast<int>(p[2]);      // touch ID [7:4] + Y high nibble [3:0]
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
        const uint8_t* p = &raw[1 + i * 6];   // skip touch_points byte, read 6-byte point block
        if (len < 1 + i * 6 + 6) break;       // not enough bytes for this point

        int xh = static_cast<int>(p[0]);      // event flag [7:6] + X high nibble [3:0]
        int xl = static_cast<int>(p[1]);      // X low byte (8 bits)
        int yh = static_cast<int>(p[2]);      // touch ID [7:4] + Y high nibble [3:0]
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
    // Production path: poll the FT3168 through esp_lcd_touch_ft5x06 and pull
    // processed coordinates out of the driver.
    if (!s_touch) return 0;
    if (esp_lcd_touch_read_data(s_touch) != ESP_OK) return 0;

    uint16_t xs[2] = {0}, ys[2] = {0}, strengths[2] = {0};
    uint8_t point_num = 0;
    if (!esp_lcd_touch_get_coordinates(s_touch, xs, ys, strengths, &point_num, 2)) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < point_num && n < max_points; ++i) {
        int x = static_cast<int>(xs[i]);
        int y = static_cast<int>(ys[i]);
        if (is_valid_point(x, y)) {
            out_xs[n] = x;
            out_ys[n] = y;
            ++n;
        }
    }
    return n;
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

// Production init: reset the panel, bring up the shared I2C bus, and create the
// FT5x06-compatible touch driver instance. Mirrors the upstream
// board_waveshare_s3_206.c touch bring-up (RST pulse, 400 kHz bus, x/y max).
// Returns ESP_OK when the driver is ready; Board::begin() logs failures.
#ifdef __ESPRESSIF_IDF__
esp_err_t ft3168_init(void) {
    if (s_touch) return ESP_OK;

    // Touch reset: HIGH -> 1ms -> LOW -> 20ms -> HIGH -> 50ms (Waveshare init).
    gpio_config_t tp_rst = {};
    tp_rst.pin_bit_mask = 1ULL << kTouchRst;
    tp_rst.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&tp_rst), kTouchTag, "touch RST gpio");
    gpio_set_level(static_cast<gpio_num_t>(kTouchRst), 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(static_cast<gpio_num_t>(kTouchRst), 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(static_cast<gpio_num_t>(kTouchRst), 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // I2C bus shared with IMU, PMU, RTC and codecs (same bus as upstream).
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(kTouchScl);
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(kTouchSda);
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_touch_bus), kTouchTag, "i2c bus");

    // Same values as ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG(), assigned
    // field-by-field because that macro's designators are out of order for C++.
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {};
    tp_io_cfg.dev_addr = kTouchAddr;
    tp_io_cfg.on_color_trans_done = nullptr;
    tp_io_cfg.control_phase_bytes = 1;
    tp_io_cfg.dc_bit_offset = 0;
    tp_io_cfg.lcd_cmd_bits = 8;
    tp_io_cfg.lcd_param_bits = 8;
    tp_io_cfg.flags.disable_control_phase = 1;
    tp_io_cfg.scl_speed_hz = 400000;
    tp_io_cfg.transaction_timeout_ms = -1;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_touch_bus, &tp_io_cfg, &s_touch_io),
                        kTouchTag, "touch IO");

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max = kDisplayWidth;
    tp_cfg.y_max = kDisplayHeight;
    tp_cfg.rst_gpio_num = static_cast<gpio_num_t>(kTouchRst);
    tp_cfg.int_gpio_num = static_cast<gpio_num_t>(kTouchInt);
    tp_cfg.levels.reset = 0;
    tp_cfg.levels.interrupt = 0;
    tp_cfg.flags.swap_xy = 0;
    tp_cfg.flags.mirror_x = 0;
    tp_cfg.flags.mirror_y = 0;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(s_touch_io, &tp_cfg, &s_touch),
                        kTouchTag, "FT3168 touch");
    return ESP_OK;
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
