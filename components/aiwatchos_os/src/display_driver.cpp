// CO5300 QSPI display driver for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
// Pin map (from board_waveshare_s3_206.c):
//   QSPI: CS=GPIO12, SCK=GPIO11, D0-D3=GPIO4-7, RST=GPIO8, EN=GPIO13
// Resolution: 410x502 RGB565 (byte-swapped big-endian for CO5300).
// The init sequence is adapted from the Waveshare BSP v3.0.0 CO5300 commands
// documented in board_waveshare_s3_206.c, with column/page address windows set
// to 410x502 (cols 0-409, rows 0-501).
#include "aiwatchos/hal.hpp"

namespace aiwatchos {

// Display enable GPIO and QSPI pin definitions.
constexpr int kLcdCs    = 12;
constexpr int kLcdSck   = 11;
constexpr int kLcdD0    = 4;
constexpr int kLcdD1    = 5;
constexpr int kLcdD2    = 6;
constexpr int kLcdD3    = 7;
constexpr int kLcdRst   = 8;
constexpr int kLcdEn    = 13;

// CO5300 init command sequence (from board_waveshare_s3_206.c s_lcd_init[]).
// Each entry: register, payload bytes, delay_ms. The framebuffer lives in PSRAM;
// rows are flushed via a DMA bounce buffer to avoid contention with Wi-Fi/BLE.
struct Co5300InitCmd {
    uint8_t reg;
    const uint8_t* data;
    size_t len;
    uint16_t delay_ms;
};

static const uint8_t kPageSelect2[] = { 0x20 };
static const uint8_t kPowerTrim[]   = { 0x10 };
static const uint8_t kRegA0[]       = { 0xA0 };
static const uint8_t kPageSelect0[] = { 0x00 };
static const uint8_t kEnableQspi[]  = { 0x80 };
static const uint8_t kPixFmt565[]   = { 0x55 };
static const uint8_t kTeOn[]        = { 0x00 };
static const uint8_t kBrightnessCtl[] = { 0x20 };
static const uint8_t kBrightnessMax[] = { 0xFF };
static const uint8_t kVendorReg[]   = { 0xFF };

// Column address: cols 0-409 (410px wide). Little-endian half-words.
static const uint8_t kColAddr[] = { 0x00, 0x00, 0x01, 0x99 };  // 0..409
// Page address: rows 0-501 (502px tall).
static const uint8_t kRowAddr[]  = { 0x00, 0x00, 0x01, 0xF5 };  // 0..501

}  // namespace aiwatchos
