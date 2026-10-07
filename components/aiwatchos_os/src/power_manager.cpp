// AXP2101 power management driver for the Waveshare 2.06" board.
// The 2.06" uses all four PMU rails: DCDC1 (VCC3V3), ALDO1 (A3V3 audio), BLDO1 (1.2V),
// and BLDO2 (2.8V) — unlike the 1.75C which only needed DCDC1+ALDO1. No trimming.
// I2C addr: 0x34, same bus as touch/IMU/RTC/codecs.
// The PWR side key connects to AXP2101's PWRON pin (no TCA9554 expander on this model).
//
// Register map and conversion formulas are ported from the upstream
// muse-gadget-206 muse_pmu.c (Apache 2.0, Meta Platforms): STATUS1/2,
// VBAT_H/L (1 mV per count), BAT_PERCENT, and the INTSTS2 power-key edges
// with write-1-to-clear semantics.
#include "aiwatchos/hal.hpp"

#ifdef __ESPRESSIF_IDF__
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#endif

namespace aiwatchos {

constexpr uint8_t kAxp2101Addr = 0x34;   // PMU I2C address

#ifdef __ESPRESSIF_IDF__
namespace {
constexpr uint8_t kRegStatus1 = 0x00;    // bit5 VBUS good, bit3 battery present
constexpr uint8_t kRegStatus2 = 0x01;    // bits[6:5] 01 = charging
constexpr uint8_t kRegIrqLevel = 0x27;   // bits[3:2] power-key hold-to-off time
constexpr uint8_t kRegAdcEnable = 0x30;  // bit0 = battery voltage ADC
constexpr uint8_t kRegVbatH = 0x34;      // bits[4:0]; 1 mV per count with VBAT_L
constexpr uint8_t kRegVbatL = 0x35;
constexpr uint8_t kRegInten2 = 0x41;
constexpr uint8_t kRegIntsts2 = 0x49;
constexpr uint8_t kRegBatPercent = 0xA4;
constexpr uint8_t kPkeyPosEdge = (1u << 0);   // released
constexpr uint8_t kPkeyNegEdge = (1u << 1);   // pressed
constexpr uint8_t kPkeyLong = (1u << 2);
constexpr uint8_t kPkeyShort = (1u << 3);
constexpr uint8_t kPkeyAll = kPkeyPosEdge | kPkeyNegEdge | kPkeyLong | kPkeyShort;
constexpr uint8_t kPkeyOff10s = (3u << 2);    // 10 s hold-to-off: hardware fallback if wedged

const char* kPmuTag = "axp2101";
i2c_master_dev_handle_t s_axp = nullptr;
bool s_pwr_pressed = false;

esp_err_t axp_rd(uint8_t reg, uint8_t* val) {
    return i2c_master_transmit_receive(s_axp, &reg, 1, val, 1, 50);
}

esp_err_t axp_wr(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_axp, buf, sizeof(buf), 50);
}
}  // namespace

// Attach the AXP2101 to the shared bus and arm the power-key edge IRQs.
// Returns ESP_OK when the PMU answers; Board::begin() logs failures and the
// driver reports "unknown" power state until init succeeds.
esp_err_t axp2101_init(void) {
    if (s_axp) return ESP_OK;
    i2c_master_bus_handle_t bus = aiwatchos_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = kAxp2101Addr;
    dev_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_axp), kPmuTag, "add device");

    uint8_t v = 0;
    ESP_RETURN_ON_ERROR(axp_rd(kRegIrqLevel, &v), kPmuTag, "AXP2101 not responding");
    ESP_RETURN_ON_ERROR(axp_wr(kRegIrqLevel, (v & ~0x0Cu) | kPkeyOff10s), kPmuTag, "set off time");
    ESP_RETURN_ON_ERROR(axp_rd(kRegInten2, &v), kPmuTag, "read inten2");
    ESP_RETURN_ON_ERROR(axp_wr(kRegInten2, v | kPkeyAll), kPmuTag, "enable key irqs");
    ESP_RETURN_ON_ERROR(axp_wr(kRegIntsts2, kPkeyAll), kPmuTag, "clear key irqs");
    ESP_RETURN_ON_ERROR(axp_rd(kRegAdcEnable, &v), kPmuTag, "read adc enable");
    ESP_RETURN_ON_ERROR(axp_wr(kRegAdcEnable, v | 0x01), kPmuTag, "enable battery voltage");
    return ESP_OK;
}

// Power-key event bits latched since the last call (write-1-to-clear).
unsigned axp2101_poll_key(void) {
    uint8_t sts = 0;
    if (!s_axp || axp_rd(kRegIntsts2, &sts) != ESP_OK) return 0;
    sts &= kPkeyAll;
    if (!sts) return s_pwr_pressed ? 2u : 0u;   // no edge: report held level
    axp_wr(kRegIntsts2, sts);
    unsigned ev = 0;
    if (sts & kPkeyNegEdge) { ev |= 1u; s_pwr_pressed = true; }
    if (sts & kPkeyPosEdge) { s_pwr_pressed = false; }
    if (sts & kPkeyShort) ev |= 4u;
    if (sts & kPkeyLong) ev |= 8u;
    if (s_pwr_pressed) ev |= 2u;
    return ev;
}

// Battery status is read from AXP2101 fuel-gauge registers over I2C.
PowerStatus axp2101_read_power() {
    PowerStatus ps{};
    if (!s_axp) {
        // PMU not initialized (or test build): report unknown, not a fake 50%.
        ps.battery_present = false;
        ps.battery_percent = 255;
        ps.battery_mv = 0;
        return ps;
    }
    uint8_t s1 = 0, s2 = 0, pct = 0, hi = 0, lo = 0;
    if (axp_rd(kRegStatus1, &s1) != ESP_OK || axp_rd(kRegStatus2, &s2) != ESP_OK) {
        ps.battery_present = false;
        ps.battery_percent = 255;
        return ps;
    }
    ps.battery_present = (s1 & (1u << 3)) != 0;
    ps.external_power = (s1 & (1u << 5)) != 0;
    ps.charging = ps.battery_present && (((s2 >> 5) & 0x3) == 0x1);
    ps.battery_percent = 255;
    ps.battery_mv = 0;
    if (ps.battery_present && axp_rd(kRegBatPercent, &pct) == ESP_OK && pct <= 100) {
        ps.battery_percent = pct;
    }
    if (ps.battery_present && axp_rd(kRegVbatH, &hi) == ESP_OK &&
        axp_rd(kRegVbatL, &lo) == ESP_OK) {
        ps.battery_mv = static_cast<uint16_t>(((hi & 0x1F) << 8) | lo);   // 1 mV per count
    }
    return ps;
}

#else  // !__ESPRESSIF_IDF__

// Battery status is read from AXP2101 fuel-gauge registers over I2C.
PowerStatus axp2101_read_power() {
    PowerStatus ps{};
    ps.battery_present = true;
    ps.battery_percent = 50;   // stub: real impl reads CONV_END register
    ps.battery_mv = 3700;      // 3.7 V nominal LiPo (MX1.25 connector)
    ps.charging = false;
    ps.external_power = false;
    return ps;
}

#endif  // __ESPRESSIF_IDF__

}  // namespace aiwatchos
