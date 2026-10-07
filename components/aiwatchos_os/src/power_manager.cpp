// AXP2101 power management driver for the Waveshare 2.06" board.
// The 2.06" uses all four PMU rails: DCDC1 (VCC3V3), ALDO1 (A3V3 audio), BLDO1 (1.2V),
// and BLDO2 (2.8V) — unlike the 1.75C which only needed DCDC1+ALDO1. No trimming.
// I2C addr: 0x34, same bus as touch/IMU/RTC/codecs.
// The PWR side key connects to AXP2101's PWRON pin (no TCA9554 expander on this model).
#include "aiwatchos/hal.hpp"

namespace aiwatchos {

constexpr uint8_t kAxp2101Addr = 0x34;   // PMU I2C address

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

}  // namespace aiwatchos
