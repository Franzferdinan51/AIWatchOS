// PCF85063 RTC driver for the Waveshare 2.06" board.
// I2C addr: 0x51, same shared bus as touch/PMU/codecs (see hal.hpp).
//
// The pure conversions (BCD, civil <-> epoch, register pack/unpack) are
// platform-independent and host-tested; only the register transport at the
// bottom touches I2C. Register layout per the PCF85063 datasheet:
//   0x04 seconds (BCD, bit 7 = OS oscillator-stop flag)
//   0x05 minutes (BCD)        0x06 hours (BCD, 24-hour mode)
//   0x07 days (BCD)           0x08 weekdays (0 = Sunday .. 6 = Saturday)
//   0x09 months (BCD)         0x0A years (BCD, 00..99 -> 2000..2099)
#include "aiwatchos/hal.hpp"   // first: carries the ESP_PLATFORM -> __ESPRESSIF_IDF__ mapping
#include "aiwatchos/rtc.hpp"

#include <cstring>

#ifdef __ESPRESSIF_IDF__
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#endif

namespace aiwatchos {

namespace {

// Days since 1970-01-01 for a Gregorian date, and the inverse
// (Howard Hinnant's civil calendar algorithms).
int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                                // [0, 399]
    const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;        // [0, 146096]
    return era * 146097 + doe - 719468;
}

void civil_from_days(int64_t z, int* y, int* m, int* d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;                             // [0, 146096]
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);      // [0, 365]
    const int64_t mp = (5 * doy + 2) / 153;                           // [0, 11]
    *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);              // [1, 31]
    *m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);                  // [1, 12]
    *y = static_cast<int>(yy + (*m <= 2 ? 1 : 0));
}

bool leap_year(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

}  // namespace

// --- BCD codec ---

uint8_t rtc_to_bcd(uint8_t value) {
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

uint8_t rtc_from_bcd(uint8_t bcd) {
    return static_cast<uint8_t>(((bcd >> 4) * 10) + (bcd & 0x0F));
}

bool rtc_bcd_valid(uint8_t bcd) {
    return (bcd & 0x0F) <= 9 && (bcd >> 4) <= 9;
}

// --- Validity ---

int rtc_days_in_month(int year, int month) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && leap_year(year)) return 29;
    return kDays[month - 1];
}

bool rtc_time_valid(const RtcTime& t) {
    if (t.year < 2024 || t.year > 2099) return false;   // pre-2024 = never set
    if (t.month < 1 || t.month > 12) return false;
    if (t.day < 1 || t.day > rtc_days_in_month(t.year, t.month)) return false;
    if (t.weekday > 6) return false;
    if (t.hour > 23 || t.minute > 59 || t.second > 59) return false;
    return true;
}

// --- Civil <-> epoch ---

uint64_t rtc_epoch_ms_from_civil(const RtcTime& t) {
    const int64_t days = days_from_civil(t.year, t.month, t.day);
    const int64_t secs = days * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
    return static_cast<uint64_t>(secs * 1000);
}

RtcTime rtc_civil_from_epoch_ms(uint64_t epoch_ms) {
    const int64_t total_secs = static_cast<int64_t>(epoch_ms / 1000);
    const int64_t days = total_secs / 86400;
    const int64_t rem = total_secs - days * 86400;                    // [0, 86399]
    RtcTime t;
    int y = 0, mo = 0, d = 0;
    civil_from_days(days, &y, &mo, &d);
    t.year = y;
    t.month = static_cast<uint8_t>(mo);
    t.day = static_cast<uint8_t>(d);
    t.weekday = static_cast<uint8_t>((days + 4) % 7);   // 1970-01-01 = Thursday
    t.hour = static_cast<uint8_t>(rem / 3600);
    t.minute = static_cast<uint8_t>((rem % 3600) / 60);
    t.second = static_cast<uint8_t>(rem % 60);
    return t;
}

// --- Register pack/unpack ---

void rtc_pack_registers(const RtcTime& t, uint8_t out[7]) {
    out[0] = rtc_to_bcd(t.second);   // OS bit (7) written 0: clears the flag
    out[1] = rtc_to_bcd(t.minute);
    out[2] = rtc_to_bcd(t.hour);     // 24-hour mode
    out[3] = rtc_to_bcd(t.day);
    out[4] = static_cast<uint8_t>(t.weekday & 0x07);
    out[5] = rtc_to_bcd(t.month);
    out[6] = rtc_to_bcd(static_cast<uint8_t>(t.year - 2000));
}

bool rtc_unpack_registers(const uint8_t in[7], RtcTime* out, bool* oscillator_stopped) {
    if (!out) return false;
    if (oscillator_stopped) *oscillator_stopped = (in[0] & 0x80) != 0;
    const uint8_t sec_bcd = in[0] & 0x7F;    // strip OS flag
    const uint8_t min_bcd = in[1] & 0x7F;
    const uint8_t hour_bcd = in[2] & 0x3F;
    const uint8_t day_bcd = in[3] & 0x3F;
    const uint8_t mon_bcd = in[5] & 0x1F;
    const uint8_t year_bcd = in[6];
    if (!rtc_bcd_valid(sec_bcd) || !rtc_bcd_valid(min_bcd) ||
        !rtc_bcd_valid(hour_bcd) || !rtc_bcd_valid(day_bcd) ||
        !rtc_bcd_valid(mon_bcd) || !rtc_bcd_valid(year_bcd)) {
        return false;
    }
    RtcTime t;
    t.second = rtc_from_bcd(sec_bcd);
    t.minute = rtc_from_bcd(min_bcd);
    t.hour = rtc_from_bcd(hour_bcd);
    t.day = rtc_from_bcd(day_bcd);
    t.weekday = in[4] & 0x07;
    t.month = rtc_from_bcd(mon_bcd);
    t.year = 2000 + rtc_from_bcd(year_bcd);
    if (!rtc_time_valid(t)) return false;
    *out = t;
    return true;
}

// --- Transport + board seam ---

#ifdef __ESPRESSIF_IDF__

namespace {
constexpr uint8_t kRegSeconds = 0x04;   // first time register
const char* kRtcTag = "pcf85063";
i2c_master_dev_handle_t s_rtc = nullptr;

esp_err_t rtc_rd(uint8_t reg, uint8_t* dst, size_t len) {
    return i2c_master_transmit_receive(s_rtc, &reg, 1, dst, len, 50);
}

esp_err_t rtc_wr(uint8_t reg, const uint8_t* src, size_t len) {
    uint8_t buf[8] = {};
    buf[0] = reg;
    std::memcpy(buf + 1, src, len);
    return i2c_master_transmit(s_rtc, buf, len + 1, 50);
}
}  // namespace

// Attach the PCF85063 to the shared bus and probe it. Returns ESP_OK when
// the chip answers; Board::begin() logs failures and the clock keeps
// running from the compiled base until something sets it.
esp_err_t pcf85063_init(void) {
    if (s_rtc) return ESP_OK;
    i2c_master_bus_handle_t bus = aiwatchos_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = kPcf85063Addr;
    dev_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_rtc), kRtcTag, "add device");

    uint8_t sec = 0;
    esp_err_t err = rtc_rd(kRegSeconds, &sec, 1);
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(s_rtc);
        s_rtc = nullptr;
        return err;   // chip not answering — begin() logs it as RTC init failed
    }
    if (sec & 0x80) {
        ESP_LOGW(kRtcTag, "oscillator-stop flag set: RTC time invalid until the clock is set");
    }
    return ESP_OK;
}

bool rtc_read_epoch_ms(uint64_t* out_epoch_ms) {
    if (!out_epoch_ms || !s_rtc) return false;
    uint8_t regs[7] = {};
    if (rtc_rd(kRegSeconds, regs, sizeof(regs)) != ESP_OK) return false;
    RtcTime t;
    bool oscillator_stopped = false;
    if (!rtc_unpack_registers(regs, &t, &oscillator_stopped)) return false;
    if (oscillator_stopped) return false;   // oscillator stopped: time untrusted
    *out_epoch_ms = rtc_epoch_ms_from_civil(t);
    return true;
}

bool rtc_write_epoch_ms(uint64_t epoch_ms) {
    if (!s_rtc) return false;
    const RtcTime t = rtc_civil_from_epoch_ms(epoch_ms);
    if (t.year < 2000 || t.year > 2099) return false;   // chip's storable range
    uint8_t regs[7];
    rtc_pack_registers(t, regs);
    return rtc_wr(kRegSeconds, regs, sizeof(regs)) == ESP_OK;
}

#else  // !__ESPRESSIF_IDF__

// Host builds have no I2C: the seam reports the contents injected by tests
// (via rtc_test_inject_epoch_ms / a previous write) and "absent" otherwise,
// so Board::begin()-style fallback logic behaves exactly as it would with
// a dead or unset RTC.
namespace {
bool s_test_present = false;
uint64_t s_test_epoch_ms = 0;
}  // namespace

bool rtc_read_epoch_ms(uint64_t* out_epoch_ms) {
    if (!out_epoch_ms || !s_test_present) return false;
    *out_epoch_ms = s_test_epoch_ms;
    return true;
}

bool rtc_write_epoch_ms(uint64_t epoch_ms) {
    const RtcTime t = rtc_civil_from_epoch_ms(epoch_ms);
    if (t.year < 2000 || t.year > 2099) return false;
    s_test_epoch_ms = epoch_ms;   // a written RTC keeps time from then on
    s_test_present = true;
    return true;
}

void rtc_test_inject_epoch_ms(uint64_t epoch_ms) {
    s_test_epoch_ms = epoch_ms;
    s_test_present = true;
}

void rtc_test_set_present(bool present) { s_test_present = present; }

#endif  // __ESPRESSIF_IDF__

}  // namespace aiwatchos
