// AIWatchOS hardware abstraction layer. Defines the interface that all board
// drivers implement; the 2.06" AMOLED board provides concrete implementations
// for CO5300 display, FT3168 touch, ES8311/ES7210 audio, AXP2101 power.
#pragma once

#include <cstdint>
#include <string>

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
    virtual bool mic_start(uint32_t sample_rate_hz) = 0;
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

// The concrete board implementation for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
// Implemented in src/hal.cpp and the driver source files. This is a singleton
// accessed via hal().
class Board : public Hal {
 public:
    static Board& instance();

    // One-time hardware init called from app_main before any other OS code.
    void begin();

    bool display_flush() override;
    void set_backlight(uint8_t percent) override;
    int read_touch(int* out_xs, int* out_ys) override;
    bool audio_start_playback(uint32_t sample_rate_hz) override;
    void audio_write(const int16_t* samples, size_t count) override;
    void audio_stop_playback() override;
    bool mic_start(uint32_t sample_rate_hz) override;
    void mic_stop() override;
    PowerStatus read_power() override;
    uint64_t now_ms() override;
    uint32_t read_buttons() override;

 private:
    Board();
};

// Global accessor for the board singleton.
Hal& hal();

}  // namespace aiwatchos
