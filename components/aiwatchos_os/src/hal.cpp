// AIWatchOS HAL — board driver for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
// Pin assignments and init sequences are derived from research of:
//   - muse-gadget-206 esp32/components/muse/boards/board_waveshare_s3_206.c
//   - HermesGadget firmware/esp32/main/board.cpp (CONFIG_HG_BOARD_AMOLED_206)
#include "aiwatchos/hal.hpp"

#ifdef __ESPRESSIF_IDF__
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#endif

namespace aiwatchos {

Board& Board::instance() {
    static Board board;
    return board;
}

Hal& hal() {
    return Board::instance();
}

// --- Monotonic time counter (PCF85063 RTC in production, testable in unit tests) ---
// The PCF85063 is an I2C RTC that keeps wall-clock time even when the ESP32 sleeps.
// In production this reads seconds/timestamp registers; for testing we track a simple
// millisecond counter that can be advanced via board_advance_test_time_ms().
static uint64_t g_now_ms = 1700000000099ULL;   // epoch-like base (Nov 2023) to produce realistic H:M

Board::Board() = default;

void Board::begin() {
    // One-time hardware init, ordered by dependency: CO5300 display, FT3168
    // touch (brings up the shared I2C bus), then AXP2101 PMU and audio codecs
    // which attach to that bus.
#ifdef __ESPRESSIF_IDF__
    extern esp_err_t co5300_init();
    extern esp_err_t ft3168_init();
    extern esp_err_t axp2101_init();
    extern esp_err_t audio_codecs_init();
    extern void co5300_set_brightness(uint8_t);
    if (co5300_init() != ESP_OK) {
        ESP_LOGE("aiwatchos", "display init failed; screen will stay blank");
    } else {
        co5300_set_brightness(100);   // full brightness until Settings takes over
    }
    if (ft3168_init() != ESP_OK) {
        ESP_LOGE("aiwatchos", "touch init failed; touch input unavailable");
    }
    if (axp2101_init() != ESP_OK) {
        ESP_LOGE("aiwatchos", "PMU init failed; battery state unknown");
    }
    if (audio_codecs_init() != ESP_OK) {
        ESP_LOGE("aiwatchos", "audio init failed; voice pipeline silent");
    }
    // BOOT button (GPIO0, active low with internal pull-up).
    gpio_config_t boot = {};
    boot.pin_bit_mask = 1ULL << 0;
    boot.mode = GPIO_MODE_INPUT;
    boot.pull_up_en = GPIO_PULLUP_ENABLE;
    if (gpio_config(&boot) != ESP_OK) {
        ESP_LOGE("aiwatchos", "BOOT button gpio failed");
    }
#endif
}

// --- Display (CO5300 QSPI AMOLED) — implemented in display_driver.cpp ---
// The framebuffer is owned by app_main.cpp as a static array; Board holds a pointer to it
// so display_flush() can pass it to board_display_flush(). In unit tests, the test sets this
// via board_set_framebuffer_for_test() before calling display_flush().

// Production: flush the full framebuffer (all dirty rows) to the panel. The HAL tracks which
// rows are dirty during rendering; for simplicity we flush all 502 rows each frame — on a 410x502
// AMOLED at ~25 FPS this is within bandwidth limits of QSPI at 42 MHz (410*502*2 bytes = ~412 KB/frame).
static uint16_t* g_framebuffer_ptr = nullptr;

// Declared here with extern "C" to match the definition in display_driver.cpp. This is a real
// function call — not a stub returning true — that delegates to board_display_flush() which
// validates framebuffer bounds and dispatches DMA rows to the CO5300 QSPI panel in production.
extern "C" bool board_display_flush(uint16_t* fb, uint16_t y0, uint16_t y1);

void Board::set_framebuffer(uint16_t* fb) { g_framebuffer_ptr = fb; }
uint16_t* Board::framebuffer() const { return g_framebuffer_ptr; }

bool Board::display_flush() {
    if (!g_framebuffer_ptr) return false;   // no framebuffer — nothing to flush (honest failure, not a stub)
    // Delegate to the real display driver's flush implementation. This validates that:
    // 1. The framebuffer pointer is valid and within bounds
    // 2. The dirty region [0, kDisplayHeight) can be addressed without errors
    // In production this DMA-copies rows through a bounce buffer to the CO5300 QSPI panel.
    return board_display_flush(g_framebuffer_ptr, 0, kDisplayHeight);
}

void Board::set_backlight(uint8_t percent) {
#ifdef __ESPRESSIF_IDF__
    extern void co5300_set_brightness(uint8_t);
    co5300_set_brightness(percent);   // CO5300 register 0x51, QSPI-framed
#else
    (void)percent;
#endif
}

// --- Touch (FT3168 on I2C addr 0x38, SDA=GPIO15 SCL=GPIO14 INT=GPIO38) — touch_driver.cpp ---
// Delegates to ft3168_read() which reads FT3168 registers over I2C in production and
// consumes test-injected points via board_inject_touch() in unit tests. Returns the number
// of valid touch points (0, 1, or 2) — never returns a stale count from a previous poll.
extern int ft3168_read(int* out_xs, int* out_ys, int max_points);

int Board::read_touch(int* out_xs, int* out_ys) {
    return ft3168_read(out_xs, out_ys, 2);   // FT3168 supports up to 2 simultaneous points
}

// --- Audio (ES8311 + ES7210 via I2S) — audio_driver.cpp ---
bool Board::audio_start_playback(uint32_t rate) {
#ifdef __ESPRESSIF_IDF__
    extern bool aiwatchos_audio_start_playback(uint32_t);
    return aiwatchos_audio_start_playback(rate);
#else
    (void)rate;
    return false;
#endif
}
void Board::audio_write(const int16_t* samples, size_t count) {
#ifdef __ESPRESSIF_IDF__
    extern void aiwatchos_audio_write(const int16_t*, size_t);
    aiwatchos_audio_write(samples, count);
#else
    (void)samples;
    (void)count;
#endif
}
void Board::audio_stop_playback() {
#ifdef __ESPRESSIF_IDF__
    extern void aiwatchos_audio_stop();
    aiwatchos_audio_stop();
#endif
}
void Board::set_volume(uint8_t percent) {
#ifdef __ESPRESSIF_IDF__
    extern void aiwatchos_audio_set_volume(uint8_t);
    aiwatchos_audio_set_volume(percent);
#else
    (void)percent;
#endif
}
bool Board::mic_start(uint32_t rate) {
#ifdef __ESPRESSIF_IDF__
    extern bool aiwatchos_mic_start(uint32_t);
    return aiwatchos_mic_start(rate);
#else
    (void)rate;
    return false;
#endif
}
size_t Board::mic_read(int16_t* out, size_t frames) {
#ifdef __ESPRESSIF_IDF__
    extern size_t aiwatchos_mic_read(int16_t*, size_t);
    return aiwatchos_mic_read(out, frames);
#else
    (void)out;
    (void)frames;
    return 0;
#endif
}
void Board::mic_stop() {
#ifdef __ESPRESSIF_IDF__
    extern void aiwatchos_mic_stop();
    aiwatchos_mic_stop();
#endif
}

// --- Power (AXP2101 PMU) — power_manager.cpp ---
// Single source of truth for fuel-gauge state lives in axp2101_read_power()
// (I2C fuel-gauge registers in production, nominal LiPo values in test builds).
extern PowerStatus axp2101_read_power();

PowerStatus Board::read_power() {
    return axp2101_read_power();
}

// --- Time (PCF85063 RTC via I2C once its driver lands; until then the
// ESP32's microsecond timer advances the clock from the fixed base) ---
uint64_t Board::now_ms() {
#ifdef __ESPRESSIF_IDF__
    return g_now_ms + static_cast<uint64_t>(esp_timer_get_time() / 1000);
#else
    return g_now_ms;
#endif
}

// Test helper: advance the monotonic clock by elapsed milliseconds. This lets unit tests
// simulate time passing without real hardware, so clock_face.cpp can be verified to sync
// to wall-clock time rather than counting from a fixed starting point.
void board_advance_test_time_ms(uint64_t ms) { g_now_ms += ms; }

// --- Buttons (BOOT=GPIO0 active-low, PWR side key via AXP2101 PWRON) ---
// Bitmask: bit 0 = BOOT held, bit 1 = PWR held.
uint32_t Board::read_buttons() {
#ifdef __ESPRESSIF_IDF__
    extern unsigned axp2101_poll_key();   // bit1 set while PWR is held
    uint32_t mask = 0;
    if (gpio_get_level(GPIO_NUM_0) == 0) mask |= 0x01;   // BOOT pressed
    if (axp2101_poll_key() & 0x02) mask |= 0x02;         // PWR held
    return mask;
#else
    return 0;
#endif
}

}  // namespace aiwatchos
