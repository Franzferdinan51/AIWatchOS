// AIWatchOS design tokens — "Muse Gloss" v1.0 (docs/ui/DESIGN_TOKENS.md).
// Every UI color in the OS and apps is one of the named constants below;
// raw hex in app code is a review defect. All values are CANONICAL
// (unswapped) RGB565: the single byte-swap for the CO5300's big-endian
// wire format happens in the display flush (display_driver.cpp), never
// in app constants and never in the framebuffer.
#pragma once

#include <cstdint>

namespace aiwatchos {

// Pack 8-bit channels into canonical RGB565 (same formula as hg::rgb565).
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Pack a 0xRRGGBB literal into canonical RGB565.
constexpr uint16_t rgb565_hex(uint32_t rgb) {
    return rgb565(static_cast<uint8_t>(rgb >> 16),
                  static_cast<uint8_t>(rgb >> 8),
                  static_cast<uint8_t>(rgb));
}

// Per-channel linear interpolation: t in [0, 256], 0 = a, 256 = b.
constexpr uint16_t lerp565(uint16_t a, uint16_t b, int t) {
    if (t <= 0) return a;
    if (t >= 256) return b;
    const int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    const int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    const int r = ar + ((br - ar) * t >> 8);
    const int g = ag + ((bg - ag) * t >> 8);
    const int bl = ab + ((bb - ab) * t >> 8);
    return static_cast<uint16_t>((r << 11) | (g << 5) | bl);
}

// Alpha-blend fg over dst with 8-bit alpha (0 = dst, 255 = fg).
constexpr uint16_t blend565(uint16_t dst, uint16_t fg, uint8_t alpha) {
    if (alpha == 0) return dst;
    if (alpha == 255) return fg;
    const int a = alpha;
    const int r = (((fg >> 11) & 0x1F) * a + ((dst >> 11) & 0x1F) * (255 - a)) / 255;
    const int g = (((fg >> 5) & 0x3F) * a + ((dst >> 5) & 0x3F) * (255 - a)) / 255;
    const int b = ((fg & 0x1F) * a + (dst & 0x1F) * (255 - a)) / 255;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

// Colour at row y of a vertical 3-stop gradient of height h (recipe G1):
// rows [0, h/2) lerp top -> mid, rows [h/2, h) lerp mid -> bot. Pure
// function of (y, h) so fills can compute per 4-row band with no LUT
// storage and no per-pixel lerp.
constexpr uint16_t vgrad_color(uint16_t top, uint16_t mid, uint16_t bot,
                               int y, int h) {
    if (h <= 1) return top;
    if (y < 0) y = 0;
    if (y >= h) y = h - 1;
    const int half = h / 2;
    if (y < half) {
        return lerp565(top, mid, half > 0 ? (y * 256) / half : 256);
    }
    const int span = h - half;
    return lerp565(mid, bot, span > 0 ? ((y - half) * 256) / span : 256);
}

// --- Muse dialect (DESIGN_TOKENS section 1) ---
namespace colors {
constexpr uint16_t kBlue      = 0x1BBE;   // #1877F2 primary chrome
constexpr uint16_t kBlueDeep  = 0x09F0;   // #0A3F86
constexpr uint16_t kBlueTop   = 0x4CBE;   // #4B95F5 filled-bubble top stop
constexpr uint16_t kBlueBot   = 0x133A;   // #1467D4 filled-bubble bottom stop
constexpr uint16_t kBubbleTop = 0x3A8E;   // #3A5174 surface-bubble top stop
constexpr uint16_t kBubble    = 0x118B;   // #14305A THE surface colour
constexpr uint16_t kBubbleBot = 0x11AC;   // #123465 surface-bubble bottom stop
constexpr uint16_t kInk       = 0x0083;   // #07101C page background base
constexpr uint16_t kNight     = 0x08C5;   // #0C1828 app background
constexpr uint16_t kInput     = 0x1107;   // #13233C input fill
constexpr uint16_t kRingTrack = 0x0908;   // #0A2343 bezel/progress track
constexpr uint16_t kText      = 0xF77F;   // #F2EFFF primary text
constexpr uint16_t kTextNum   = 0xFFFF;   // #FFFFFF headline numerals only
constexpr uint16_t kCaption   = 0xDE9F;   // #D8D2FF caption/reply text
constexpr uint16_t kDim       = 0x8C35;   // #8B84A8 secondary text
constexpr uint16_t kFaint     = 0x398B;   // #3A3358 tertiary / off segments
constexpr uint16_t kOk        = 0x6F97;   // #6FF0BF success / connected
constexpr uint16_t kWarn      = 0xFDAB;   // #FFB45C warning
constexpr uint16_t kDanger    = 0xFAEB;   // #FF5C5C error / destructive
constexpr uint16_t kMist      = 0xF7DF;   // #F4F8FF text on filled blue
constexpr uint16_t kBlack     = 0x0000;
constexpr uint16_t kWhite     = 0xFFFF;
}  // namespace colors

// --- Jolly avatar fixed palette (DESIGN_TOKENS section 2) ---
// The character never changes colour with state; only its glow does.
namespace jolly {
constexpr uint16_t kFurLight = 0xFF7B;   // #F8EEDC
constexpr uint16_t kFurMid   = 0xE6B7;   // #E6D7BD
constexpr uint16_t kFurDark  = 0xACCF;   // #AE987E
constexpr uint16_t kSkin     = 0xF6F7;   // #F6DFBD
constexpr uint16_t kSkinLight = 0xFF7A;  // #FDEED6
constexpr uint16_t kBlush    = 0xF554;   // #F4AAA0
constexpr uint16_t kHeart    = 0xFA71;   // #FF4F8B
constexpr uint16_t kOutline  = 0x3944;   // #3A2B22
}  // namespace jolly

// --- Muse state system (DESIGN_TOKENS section 3) ---
// Each state owns a 4-stop glow ramp (f0 brightest -> f3 deep) + one
// accent. The accent may appear ONLY in: the state word, the ring
// indicator/spinner, the level meter, the avatar glow, an 8px status
// dot. Chrome (bezel, bubbles, selection) is always the blue family.
enum class MuseState : uint8_t {
    kBoot, kReady, kListening, kThinking, kSpeaking, kError, kOff, kCount
};

struct StateScheme {
    uint16_t f0, f1, f2, f3;
    uint16_t accent;
};

constexpr StateScheme kStateSchemes[static_cast<int>(MuseState::kCount)] = {
    // Boot
    {0xFFFF, 0xCF1F, 0x8D5F, 0x5AFC, 0xAE1F},
    // Ready (idle)
    {0xF75F, 0xC53F, 0x9B5F, 0x59FB, 0xA3FF},
    // Listening
    {0xEFDF, 0x8EFF, 0x3D1F, 0x2ADA, 0x5DDF},
    // Thinking
    {0xFF3F, 0xFCFE, 0xD2DF, 0x795B, 0xE3DF},
    // Speaking
    {0xEFFE, 0x9FB9, 0x3ED4, 0x1CCF, 0x6F97},
    // Error
    {0xFEBA, 0xFB4D, 0xC189, 0x68C7, 0xFAEB},
    // Off
    {0xDEBF, 0x8C3B, 0x5A76, 0x294E, 0x7B9A},
};

constexpr const StateScheme& state_scheme(MuseState s) {
    return kStateSchemes[static_cast<int>(s)];
}

// --- Hermes dialect (DESIGN_TOKENS section 9) ---
// Same geometry as Muse; palette swap only. Used by the Hermes screen
// inside AIWatchOS (gold identity) — HermesGadget keeps its own copy.
namespace hcolors {
constexpr uint16_t kGold     = 0xF587;   // #F2B33D primary chrome
constexpr uint16_t kGoldDim  = 0x6A83;   // #6E521E
constexpr uint16_t kBarTop   = 0x3A09;   // #3D434C bubble top stop
constexpr uint16_t kBar      = 0x18E5;   // #181F2A bubble / surface
constexpr uint16_t kBarBot   = 0x2964;   // #2E2C27 bubble bottom stop
constexpr uint16_t kBg       = 0x0862;   // #0A0E14 page background
constexpr uint16_t kText     = 0xEF7E;   // #E8EEF2
constexpr uint16_t kDim      = 0x8494;   // #8492A0
constexpr uint16_t kFaint    = 0x31E9;   // #343E4C
constexpr uint16_t kYes      = 0x1AE7;   // #1C5C3E prompt YES fill (dim green)
constexpr uint16_t kNo       = 0x7965;   // #7A2C2C prompt NO fill (dim red)
constexpr uint16_t kOnline   = 0x3EB1;   // #3DD68C link online / listening
constexpr uint16_t kThinking = 0x5D5E;   // #5AA9F2
constexpr uint16_t kError    = 0xF2EB;   // #F25F5C
constexpr uint16_t kWarn     = 0xF649;   // #F2C94C link net-pending
// Slate user-turn bubble ramp (ui-overhaul DESIGN §2.3): one step lifted
// from the bar ramp so user turns read as "same room, different
// speaker". Top stop intentionally equals kBarTop (#3D434C).
constexpr uint16_t kSlateTop = kBarTop;
constexpr uint16_t kSlateMid = rgb565_hex(0x272E36);
constexpr uint16_t kSlateBot = rgb565_hex(0x1B2129);
}  // namespace hcolors

}  // namespace aiwatchos
