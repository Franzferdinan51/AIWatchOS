// PCF85063 RTC — the watch's real wall clock. The chip sits on the shared I2C
// bus (SDA=GPIO15/SCL=GPIO14) and keeps time on battery while the ESP32
// sleeps or is off. Until this driver landed, Board::now_ms() advanced from
// a fixed compile-time base, so the watch face showed plausible-but-wrong
// time that drifted with uptime and reset every boot.
//
// Layering: the pure conversions (BCD codec, civil <-> epoch, register
// pack/unpack, validity) are host-testable and live in src/rtc_driver.cpp
// next to the I2C transport. The board seam below is what the rest of the OS
// uses: Board::begin() seeds the software clock from the RTC when its time
// is valid (RTC wins over any compiled base), and a future SNTP sync
// disciplines the RTC through board_set_epoch_ms().
#pragma once

#include <cstdint>

namespace aiwatchos {

// PCF85063 I2C address — fixed by the chip; matches the muse-gadget-206
// board notes for this board ("RTC: PCF85063 @ 0x51 (same I2C bus)").
constexpr uint8_t kPcf85063Addr = 0x51;

// Broken-down civil time as stored in the PCF85063 time registers.
// weekday uses the chip's convention: 0 = Sunday .. 6 = Saturday.
struct RtcTime {
    int year = 2000;      // full year, e.g. 2026 (chip stores year % 100)
    uint8_t month = 1;    // 1..12
    uint8_t day = 1;      // 1..31
    uint8_t weekday = 0;  // 0..6, 0 = Sunday
    uint8_t hour = 0;     // 0..23 (24-hour mode)
    uint8_t minute = 0;   // 0..59
    uint8_t second = 0;   // 0..59
};

// --- BCD codec (the PCF85063 stores every time field in BCD) ---
uint8_t rtc_to_bcd(uint8_t value);     // value must be 0..99
uint8_t rtc_from_bcd(uint8_t bcd);     // caller checks rtc_bcd_valid() first
bool rtc_bcd_valid(uint8_t bcd);       // both nibbles <= 9

// Days in a month, leap-year aware (proleptic Gregorian). 0 for bad month.
int rtc_days_in_month(int year, int month);

// Sane-range validity for a broken-down time. The chip's year field spans
// 2000..2099, but anything before 2024 means "never set" (fresh chip or the
// backup battery died), so it counts as invalid alongside out-of-range
// fields and impossible dates (Feb 30, Feb 29 in a non-leap year).
bool rtc_time_valid(const RtcTime& t);

// --- Civil <-> Unix epoch conversions (UTC) ---
uint64_t rtc_epoch_ms_from_civil(const RtcTime& t);   // t must be valid-range
RtcTime rtc_civil_from_epoch_ms(uint64_t epoch_ms);   // ms remainder dropped

// --- PCF85063 time register block, 0x04..0x0A ---
// (seconds, minutes, hours, days, weekdays, months, years)
// Pack emits the 7 register bytes for a set-time write; the seconds byte's
// OS (oscillator stop) bit is written 0, which clears a latched OS flag.
// Unpack masks off non-value bits, validates BCD + field ranges, and reports
// the OS bit via oscillator_stopped. A set OS bit means the oscillator
// stopped at some point (power loss): the fields may decode, but the time
// must be treated as untrusted — rtc_read_epoch_ms() rejects it.
void rtc_pack_registers(const RtcTime& t, uint8_t out[7]);
bool rtc_unpack_registers(const uint8_t in[7], RtcTime* out, bool* oscillator_stopped);

// --- Board seam ---
// Read the RTC as Unix epoch milliseconds (UTC). Returns false when the RTC
// is absent/uninitialized, its oscillator-stop flag is set, or its contents
// fail rtc_time_valid() — callers fall back to the compiled clock base.
bool rtc_read_epoch_ms(uint64_t* out_epoch_ms);
// Write Unix epoch milliseconds to the RTC (also clears the OS flag).
// Returns false when the RTC is absent/uninitialized or the value falls
// outside the chip's 2000..2099 range.
bool rtc_write_epoch_ms(uint64_t epoch_ms);

// Set the board's wall clock to epoch_ms: rebases Board::now_ms() and
// writes through to the PCF85063 so the time survives sleep and reboot.
// This is the entry point a future SNTP sync calls once it has network
// time. Defined in hal.cpp, next to the clock base it adjusts.
void board_set_epoch_ms(uint64_t epoch_ms);

// Test-only: host builds have no I2C, so the seam reports "absent" unless a
// test injects contents. rtc_test_inject_epoch_ms() sets the value reads
// return and marks the RTC present; rtc_test_set_present(false) restores
// the absent behavior. Mirrors board_inject_touch()/board_advance_test_time_ms().
void rtc_test_inject_epoch_ms(uint64_t epoch_ms);
void rtc_test_set_present(bool present);

}  // namespace aiwatchos
