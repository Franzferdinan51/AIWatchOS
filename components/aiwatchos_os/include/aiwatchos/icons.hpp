// Icon codepoints (Private Use Area) baked by tools/gen_font.py into
// the icon faces. This table MUST match ICON_NAMES order there.
#pragma once

#include <cstdint>

namespace aiwatchos {

enum class Icon : uint32_t {
    kMic = 0xE001,
    kFeather,
    kSparkle,
    kGear,
    kBell,
    kClock,
    kSun,
    kCloud,
    kCloudSun,
    kCloudRain,
    kCloudSnow,
    kCheck,
    kChevronRight,
    kWifi,
    kWifiOff,
    kMoon,
    kVolume,
    kInfo,
    kAlert,
    kX,
    kArrowUp,
    kArrowDown,
    kHeart,
    kPlay,
    kPause,
    kMusic,
    kSend,
    kDot,
};

}  // namespace aiwatchos
