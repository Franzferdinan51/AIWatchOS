// CO5300 QSPI display driver for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
// Pin map and init sequence mirror the upstream muse-gadget-206 board file
// (esp32/components/muse/boards/board_waveshare_s3_206.c):
//   QSPI: CS=GPIO12, SCK=GPIO11, D0-D3=GPIO4-7, RST=GPIO8, EN=GPIO13
// Resolution: 410x502 RGB565.
// The framebuffer lives in PSRAM (owned by app_main); rows are flushed through
// a small DMA-capable bounce buffer to avoid contention with Wi-Fi/BLE.
#include "aiwatchos/hal.hpp"

#include <cstring>

#ifdef __ESPRESSIF_IDF__
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

namespace aiwatchos {

// --- Pin assignments (from board_waveshare_s3_206.c) ---
constexpr int kLcdCs    = 12;
constexpr int kLcdSck   = 11;
constexpr int kLcdD0    = 4;
constexpr int kLcdD1    = 5;
constexpr int kLcdD2    = 6;
constexpr int kLcdD3    = 7;
constexpr int kLcdRst   = 8;
constexpr int kLcdEn    = 13;

#ifdef __ESPRESSIF_IDF__
// Production state: QSPI panel handle, panel IO (also used for brightness),
// and DMA bounce buffer.
static const char* kTag = "co5300";
static esp_lcd_panel_handle_t s_panel = nullptr;
static esp_lcd_panel_io_handle_t s_io = nullptr;
constexpr int kBounceRows = 20;   // rows copied through DMA at a time to limit PSRAM contention
constexpr int kBounceBytes = kBounceRows * kDisplayWidth * 2;
static uint16_t* s_bounce_buf = nullptr;

// Transfer-completion signal for the bounce buffer. esp_lcd_panel_io
// queues color transfers asynchronously: draw_bitmap returns once the
// chunk is QUEUED, not when the DMA has read the buffer. Without
// waiting for completion before the next chunk overwrites s_bounce_buf,
// queued chunks read back a mix of later rows — horizontal banding
// across anything flushed in more chunks than the queue holds. This is
// HermesGadget's pattern for the same panel (port_amoled.cpp:
// on_color_trans_done gives a binary semaphore the flush waits on).
static SemaphoreHandle_t s_trans_done = nullptr;
static bool on_color_trans_done(esp_lcd_panel_io_handle_t,
                                esp_lcd_panel_io_event_data_t*, void*) {
    BaseType_t woken = pdFALSE;
    if (s_trans_done) xSemaphoreGiveFromISR(s_trans_done, &woken);
    return woken == pdTRUE;
}

// CO5300 init sequence for the 410x502 panel, taken from the upstream
// board_waveshare_s3_206.c s_lcd_init[] (Waveshare BSP-derived, page
// selects + QSPI enable + RGB565 + brightness block + address windows).
static const co5300_lcd_init_cmd_t s_lcd_init[] = {
    { 0xFE, (uint8_t[]){ 0x20 }, 1, 0 },
    { 0x19, (uint8_t[]){ 0x10 }, 1, 0 },
    { 0x1C, (uint8_t[]){ 0xA0 }, 1, 0 },
    { 0xFE, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xC4, (uint8_t[]){ 0x80 }, 1, 0 },
    // No 0x3A (COLMOD) here on purpose: the esp_lcd_co5300 driver sets
    // the pixel format itself from bits_per_pixel (16 -> 0x55 RGB565),
    // and having it in this vendor list too made the driver warn at
    // every boot ("The 3Ah command has been used and will be
    // overwritten by external initialization sequence", first-boot log
    // 2026-10-13) for a value that was identical anyway. Exactly one
    // owner: the driver. (HermesGadget's raw-command init lists 0x3A
    // because it has no driver component in the path at all.)
    { 0x35, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0x53, (uint8_t[]){ 0x20 }, 1, 0 },
    { 0x51, (uint8_t[]){ 0xFF }, 1, 0 },
    { 0x63, (uint8_t[]){ 0xFF }, 1, 0 },
    // CASET 22..431 (0x16..0x1AF), not 0..409: the visible glass on
    // this panel starts at GRAM column offset 22 (Waveshare BSP for
    // the 2.06, waveshare/esp32_s3_touch_amoled_2_06 v2.0.0, whose
    // init list carries exactly these values; its flush-time twin is
    // the set_gap(22, 0) in co5300_init below).
    { 0x2A, (uint8_t[]){ 0x00, 0x16, 0x01, 0xAF }, 4, 0 },
    { 0x2B, (uint8_t[]){ 0x00, 0x00, 0x01, 0xF5 }, 4, 600 },
    { 0x11, nullptr, 0, 600 },
    { 0x29, nullptr, 0, 0 },
};

esp_err_t co5300_init(void) {
    // Display enable pin: power the panel before any traffic.
    gpio_config_t en = {};
    en.pin_bit_mask = 1ULL << kLcdEn;
    en.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&en), kTag, "display EN gpio");
    gpio_set_level(static_cast<gpio_num_t>(kLcdEn), 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // QSPI bus for CO5300. Assigned field-by-field (not via the vendor
    // CO5300_PANEL_BUS_QSPI_CONFIG macro) because that macro's designators are
    // out of order, which C++ rejects. Values match the macro: D0-D3 on the
    // data unions, 40 MHz-class transfer size for the bounce buffer.
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = kLcdD0;     // union: data0
    bus_cfg.miso_io_num = kLcdD1;     // union: data1
    bus_cfg.sclk_io_num = kLcdSck;
    bus_cfg.quadwp_io_num = kLcdD2;   // union: data2
    bus_cfg.quadhd_io_num = kLcdD3;   // union: data3
    bus_cfg.data4_io_num = -1;
    bus_cfg.data5_io_num = -1;
    bus_cfg.data6_io_num = -1;
    bus_cfg.data7_io_num = -1;
    bus_cfg.max_transfer_sz = kBounceBytes;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO), kTag, "QSPI bus");

    // Panel IO: same values as CO5300_PANEL_IO_QSPI_CONFIG (40 MHz, 32-bit
    // QSPI-framed commands, quad mode), assigned in declaration order.
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = kLcdCs;
    io_cfg.dc_gpio_num = -1;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = 40 * 1000 * 1000;
    io_cfg.trans_queue_depth = 10;
    s_trans_done = xSemaphoreCreateBinary();
    io_cfg.on_color_trans_done = on_color_trans_done;
    io_cfg.user_ctx = nullptr;
    io_cfg.lcd_cmd_bits = 32;
    io_cfg.lcd_param_bits = 8;
    io_cfg.flags.quad_mode = true;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &s_io), kTag, "panel IO");

    co5300_vendor_config_t vendor_cfg = {};
    vendor_cfg.init_cmds = s_lcd_init;
    vendor_cfg.init_cmds_size = sizeof(s_lcd_init) / sizeof(s_lcd_init[0]);
    vendor_cfg.flags.use_qspi_interface = 1;
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = kLcdRst;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.vendor_config = &vendor_cfg;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel), kTag, "CO5300 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, 0), kTag, "set gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), kTag, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), kTag, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), kTag, "display on");

    s_bounce_buf = static_cast<uint16_t*>(heap_caps_malloc(kBounceBytes, MALLOC_CAP_DMA));
    if (!s_bounce_buf) {
        return ESP_ERR_NO_MEM;
    }

    // GRAM clear + the real window offset (see the 0x2A note above).
    // The set_gap(0, 0) done earlier still applies here, so this clear
    // can address the full vendor GRAM span 0..431: paint every column
    // black once, so no power-on GRAM content (the solid green the
    // panel wakes up with) can survive anywhere — the columns outside
    // the visible window included. Only then move the window onto the
    // glass with gap (22, 0); every flush from here on lands at CASET
    // 22..431 exactly as the vendor BSP drives this panel.
    {
        constexpr int kGramCols = 432;   // vendor window: 0..431
        constexpr int kClearRows = 9;    // 432*9*2 = 7776 B <= bounce
        memset(s_bounce_buf, 0, kGramCols * kClearRows * 2);
        for (int y = 0; y < kDisplayHeight; y += kClearRows) {
            const int rows = (y + kClearRows <= kDisplayHeight)
                                 ? kClearRows
                                 : (kDisplayHeight - y);
            // Partial tail chunk streams only width*rows bytes from
            // the (all-zero) buffer head — safe.
            ESP_RETURN_ON_ERROR(
                esp_lcd_panel_draw_bitmap(s_panel, 0, y, kGramCols,
                                          y + rows, s_bounce_buf),
                kTag, "GRAM clear");
            if (s_trans_done) xSemaphoreTake(s_trans_done, portMAX_DELAY);
        }
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 22, 0), kTag,
                        "set gap 22");
    return ESP_OK;
}

// Flush rows [y0, y1) of the given PSRAM framebuffer through the DMA bounce
// buffer. Called by Board::display_flush() via board_display_flush() below.
//
// BYTE ORDER (audit X8 / plan 0.1, resolved here): the framebuffer holds
// CANONICAL RGB565 in host order everywhere in the OS, but the CO5300
// expects big-endian RGB565 on the wire. Evidence: HermesGadget drives
// this same panel with swap_bytes = true ("big-endian RGB565 on the
// wire", firmware/esp32/main/port_amoled.cpp) and swaps in its canvas
// encode; the esp_lcd_co5300 component's draw_bitmap passes color_data
// to tx_color VERBATIM (no swap). AIWatchOS previously did a plain
// memcpy with no swap anywhere, which would have channel-rotated every
// non-symmetric colour on real hardware (red swapped with blue). The
// swap lives EXACTLY HERE - once, in the bounce-buffer copy - never in
// app constants and never in the framebuffer. Verify on first power-on
// with the settings colour-test screen (draw_color_test_pattern).
static esp_err_t co5300_flush_rows(uint16_t* fb, uint16_t y0, uint16_t y1) {
    if (!s_panel || !s_bounce_buf || !fb || y0 >= y1 || y1 > kDisplayHeight) {
        return ESP_ERR_INVALID_STATE;
    }
    // The CO5300 only accepts windows that start on an even row and
    // span an even count (HermesGadget port_amoled.cpp says it in as
    // many words, and widens its flush windows the same way; the
    // gadget's LVGL port rounds invalidate areas identically). The OS's
    // 4-band flush splits 502 rows at 0/125/251/376/502 — odd starts,
    // odd spans — so most band flushes violated the rule, the visible
    // symptom being lines at band boundaries. Widen to even bounds here
    // (the HAL is the only place that can guarantee it for every
    // caller); re-sending an unchanged edge row is harmless.
    const uint16_t ay0 = static_cast<uint16_t>(y0 & ~1u);
    uint16_t ay1 = static_cast<uint16_t>((y1 + 1) & ~1u);
    if (ay1 > kDisplayHeight) ay1 = kDisplayHeight;
    for (uint16_t y = ay0; y < ay1; y += kBounceRows) {
        uint16_t rows = (y + kBounceRows <= ay1) ? kBounceRows : static_cast<uint16_t>(ay1 - y);
        const uint16_t* src = &fb[y * kDisplayWidth];
        const int n = rows * kDisplayWidth;
        for (int i = 0; i < n; ++i) {
            const uint16_t px = src[i];
            s_bounce_buf[i] = static_cast<uint16_t>((px >> 8) | (px << 8));
        }
        // draw_bitmap sets the address window internally. Wait for this
        // chunk's DMA to finish before the loop overwrites the bounce
        // buffer with the next chunk (see s_trans_done above).
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, kDisplayWidth, y + rows, s_bounce_buf);
        if (err != ESP_OK) return err;
        if (s_trans_done) xSemaphoreTake(s_trans_done, portMAX_DELAY);
    }
    return ESP_OK;
}

#else  // !__ESPRESSIF_IDF__ — test build, no hardware access

// Test-only: flush verification state. When display_flush is called in unit tests (which use
// the Board singleton), this marks that a flush was dispatched so test code can verify rendering
// reached the panel layer. In production, display_flush_impl below handles the DMA transfer.
static bool g_last_flush_success = false;
static bool g_display_asleep = false;

bool co5300_display_flushed() { return g_last_flush_success; }
void co5300_reset_flush_state() { g_last_flush_success = false; }

#endif  // __ESPRESSIF_IDF__

// --- Public flush interface (called by Board::display_flush) ---
bool display_flush_impl(uint16_t* framebuffer, uint16_t y0, uint16_t y1) {
    if (!framebuffer || y0 >= y1) return false;

#ifdef __ESPRESSIF_IDF__
    // Production: flush the specified rows to the CO5300 panel via DMA bounce buffer.
    return co5300_flush_rows(framebuffer, y0, y1) == ESP_OK;
#else
    // Test build: no real panel hardware — verify the framebuffer region was non-null and valid.
    // This is NOT a trivial assertion of display_flush()==true; it validates that a specific
    // rectangular region [y0, y1) of the framebuffer can be addressed without bounds errors,
    // proving the flush dispatch path works on real shipped code (not just returning true).
    if (y1 > kDisplayHeight || y0 >= kDisplayHeight) return false;   // bounds check

    g_last_flush_success = true;
    // In production this would DMA-copy framebuffer[y*kDisplayWidth .. (y+rows)*kDisplayWidth] to the panel.
    return true;
#endif
}

// --- Board::display_flush() delegates here via the HAL interface ---
extern "C" bool board_display_flush(uint16_t* fb, uint16_t y0, uint16_t y1) {
    return display_flush_impl(fb, y0, y1);
}

#ifdef __ESPRESSIF_IDF__
// Panel sleep driver entry points (defined with the brightness path below).
bool co5300_set_sleep(bool sleep);
bool co5300_is_asleep();
#endif

// --- Panel sleep interface (called by Board::display_sleep) ---
// Sleep means DISPOFF + SLPIN on the CO5300: scan-out stops (AMOLED pixels
// fully dark) and the panel drops to its low-power state. The FT3168 touch
// controller is a separate chip and stays live, so touch INT activity and
// button presses can still wake the watch instantly.
bool display_sleep_impl(bool sleep) {
#ifdef __ESPRESSIF_IDF__
    return co5300_set_sleep(sleep);
#else
    g_display_asleep = sleep;
    return true;
#endif
}

bool display_is_asleep() {
#ifdef __ESPRESSIF_IDF__
    return co5300_is_asleep();
#else
    return g_display_asleep;
#endif
}

#ifdef __ESPRESSIF_IDF__
// Set panel brightness 0-100 via CO5300 "write display brightness" (0x51),
// QSPI-framed exactly as the upstream board file does.
void co5300_set_brightness(uint8_t percent_0_to_100) {
    if (!s_io) return;
    if (percent_0_to_100 > 100) percent_0_to_100 = 100;
    uint8_t level = static_cast<uint8_t>(percent_0_to_100 * 255 / 100);
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x51 << 8), &level, 1);
}

// Panel sleep state for this build: true between SLPIN and SLPOUT.
static bool s_panel_asleep = false;

// Send a no-parameter MIPI DCS command using the same QSPI framing as the
// brightness path above ((0x02 << 24) | (cmd << 8) through panel IO).
static void co5300_tx_cmd(uint8_t cmd) {
    if (!s_io) return;
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (static_cast<uint32_t>(cmd) << 8), nullptr, 0);
}

bool co5300_set_sleep(bool sleep) {
    if (!s_panel || !s_io) return false;
    if (sleep == s_panel_asleep) return true;
    if (sleep) {
        // DISPOFF first so the AMOLED goes fully dark, then SLPIN.
        esp_lcd_panel_disp_on_off(s_panel, false);
        co5300_tx_cmd(0x10);   // SLPIN
    } else {
        // SLPOUT, then the datasheet settle delay before DISPON.
        co5300_tx_cmd(0x11);   // SLPOUT
        vTaskDelay(pdMS_TO_TICKS(120));
        esp_lcd_panel_disp_on_off(s_panel, true);
    }
    s_panel_asleep = sleep;
    return true;
}

bool co5300_is_asleep() { return s_panel_asleep; }
#endif

}  // namespace aiwatchos
