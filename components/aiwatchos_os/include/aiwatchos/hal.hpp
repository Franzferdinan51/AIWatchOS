// AIWatchOS hardware abstraction layer. Defines the interface that all board
// drivers implement; the 2.06" AMOLED board provides concrete implementations
// for CO5300 display, FT3168 touch, ES8311/ES7210 audio, AXP2101 power.
#pragma once

// ESP-IDF defines ESP_PLATFORM (not __ESPRESSIF_IDF__) for all component
// compiles. Map it so the driver guards below select production code paths in
// firmware builds and test doubles in host builds. Without this, firmware
// builds silently compiled the test paths (injected touches, stub power,
// 412 KB DRAM framebuffer) — the hardware build overflowed DRAM by ~127 KB.
#if defined(ESP_PLATFORM) && !defined(__ESPRESSIF_IDF__)
#define __ESPRESSIF_IDF__ 1
#endif

#include <cstdint>
#include <cstddef>
#include <string>

#ifdef __ESPRESSIF_IDF__
#include "driver/i2c_master.h"
#endif

namespace aiwatchos {

// Display dimensions (Waveshare ESP32-S3-Touch-AMOLED-2.06).
constexpr int kDisplayWidth = 410;
constexpr int kDisplayHeight = 502;

// Power status reported by the AXP2101 PMU.
struct PowerStatus {
    bool battery_present;
    uint8_t battery_percent;   // 0..100, or 255 if unknown
    uint16_t battery_mv;       // millivolts
    bool charging;
    bool external_power;
};

// Hardware abstraction that the OS core depends on. Each method is implemented
// by a board-specific driver in this component's src/ directory. The methods
// are non-blocking: long-running hardware work happens in background tasks or
// uses DMA, and results are polled via tick().
class Hal {
 public:
    // --- Display (CO5300 QSPI AMOLED) ---
    // Returns true if the framebuffer was flushed to the panel this call.
    virtual bool display_flush() = 0;
    virtual void set_backlight(uint8_t percent_0_to_100) = 0;

    // --- Touch (FT3168 on I2C addr 0x38, SDA=GPIO15 SCL=GPIO14 INT=GPIO38) ---
    // Returns the number of touch points read into out_points (max 2).
    virtual int read_touch(int* out_xs, int* out_ys) = 0;

    // --- Audio (ES8311 speaker + ES7210 mic via I2S) ---
    virtual bool audio_start_playback(uint32_t sample_rate_hz) = 0;
    virtual void audio_write(const int16_t* samples, size_t count) = 0;
    virtual void audio_stop_playback() = 0;
    // Speaker volume 0..100 (0 = mute). Takes effect on the next playback.
    virtual void set_volume(uint8_t percent_0_to_100) = 0;
    virtual bool mic_start(uint32_t sample_rate_hz) = 0;
    // Capture up to `frames` PCM16 mono frames into `out`; returns frames read.
    virtual size_t mic_read(int16_t* out, size_t frames) = 0;
    virtual void mic_stop() = 0;

    // --- Power (AXP2101 PMU) ---
    virtual PowerStatus read_power() = 0;

    // --- Time (PCF85063 RTC) ---
    virtual uint64_t now_ms() = 0;

    // --- Buttons ---
    // Returns bitmask of currently pressed buttons: bit 0 = BOOT, bit 1 = PWR.
    virtual uint32_t read_buttons() = 0;

 protected:
    ~Hal() = default;
};

#ifdef __ESPRESSIF_IDF__
// Shared I2C bus (I2C_NUM_0, SDA=GPIO15/SCL=GPIO14) brought up by the FT3168
// touch init. PMU, codecs and other bus clients attach devices to this bus
// instead of creating their own. Null until ft3168_init() has run.
i2c_master_bus_handle_t aiwatchos_i2c_bus();
#endif

// The concrete board implementation for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
// Implemented in src/hal.cpp and the driver source files. This is a singleton
// accessed via hal().
class Board : public Hal {
 public:
    static Board& instance();

    // One-time hardware init called from app_main before any other OS code.
    void begin();

    bool display_flush() override;   // flushes framebuffer to CO5300 panel via DMA bounce buffer
    void set_framebuffer(uint16_t* fb);  // sets the RGB565 framebuffer pointer (called by app_main)
    uint16_t* framebuffer() const;        // returns the current framebuffer pointer
    void set_backlight(uint8_t percent) override;
    int read_touch(int* out_xs, int* out_ys) override;
    bool audio_start_playback(uint32_t sample_rate_hz) override;
    void audio_write(const int16_t* samples, size_t count) override;
    void audio_stop_playback() override;
    void set_volume(uint8_t percent_0_to_100) override;
    bool mic_start(uint32_t sample_rate_hz) override;
    size_t mic_read(int16_t* out, size_t frames) override;
    void mic_stop() override;
    PowerStatus read_power() override;
    uint64_t now_ms() override;
    uint32_t read_buttons() override;

 private:
    Board();
};

// Global accessor for the board singleton.
Hal& hal();

// Test-only: advance the monotonic clock by `ms` milliseconds so that now_ms()
// returns a progressing value in unit tests (replaces PCF85063 RTC reads).
void board_advance_test_time_ms(uint64_t ms);

// Test-only: inject up to 2 touch points with screen-space coordinates. These are consumed
// by the next Board::read_touch() call, simulating one IRQ-driven poll cycle from the FT3168.
// This lets unit tests exercise the real touch dispatch path (Board -> AppManager -> LauncherUI)
// without physical hardware or a mock that always returns 0.
void board_inject_touch(int x0, int y0, int x1 = -1, int y1 = -1, int count = 1);

// Test-only: inject raw FT3168 register bytes (as received over I2C) so tests can verify the
// production parser handles realistic register formats from the datasheet. The bytes are parsed
// immediately by parse_ft3168_registers() into screen-space coordinates, then consumed on read.
void board_inject_touch_bytes(const uint8_t* raw, int len);

// Test-accessible: drives the REAL ft3168_register parsing logic (parse_ft3168_registers) with
// raw register bytes matching the FT5x06/FT3168 datasheet format. Returns count of valid points.
int ft3168_parse_test(const uint8_t* raw, int len, int* out_xs, int* out_ys, int max_points);

}  // namespace aiwatchos
