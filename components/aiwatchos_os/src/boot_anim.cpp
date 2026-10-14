// Muse boot animation — see boot_anim.hpp.
#include "aiwatchos/boot_anim.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "aiwatchos/colors.hpp"
#include "aiwatchos/font.hpp"

#ifdef __ESPRESSIF_IDF__
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace aiwatchos {
namespace {

// Baked mark (tools/gen_boot_sprite.py): "AIBM" + u16LE w/h, then
// per-pixel u8 alpha + u16LE rgb565. ~191 KB in the FAT assets
// partition, PSRAM-resident only while the animation plays.
struct BootSprite {
    int w = 0;
    int h = 0;
    const uint8_t* px = nullptr;   // 3 B/px: alpha, rgb565 LE
};
uint8_t* g_sprite_file = nullptr;
BootSprite g_sprite{};

// Phases (ms): each element fades + rises on its own window.
constexpr uint32_t kMarkStart = 0;
constexpr uint32_t kMarkLen = 600;
constexpr uint32_t kWordStart = 450;
constexpr uint32_t kWordLen = 500;
constexpr uint32_t kMetaStart = 950;
constexpr uint32_t kMetaLen = 450;
constexpr uint32_t kCreditStart = 1200;
constexpr uint32_t kCreditLen = 400;

// 0..255 ease-out ramp for [start, start+len).
int ramp(uint32_t t, uint32_t start, uint32_t len) {
    if (t <= start) return 0;
    if (t >= start + len) return 255;
    const float p = static_cast<float>(t - start) / static_cast<float>(len);
    const float e = 1.0f - (1.0f - p) * (1.0f - p);
    return static_cast<int>(e * 255.0f);
}

// Fade a foreground color toward black (the boot screen background).
uint16_t fade_to_black(uint16_t fg, int g) {
    if (g >= 255) return fg;
    if (g <= 0) return 0x0000;
    return blend565(0x0000, fg, static_cast<uint8_t>(g));
}

void draw_sprite(Framebuffer& fb, int x0, int y0, int global_a) {
    if (!g_sprite.px || global_a <= 0) return;
    for (int y = 0; y < g_sprite.h; ++y) {
        const int dy = y0 + y;
        if (dy < 0 || dy >= fb.height) continue;
        for (int x = 0; x < g_sprite.w; ++x) {
            const int dx = x0 + x;
            if (dx < 0 || dx >= fb.width) continue;
            const uint8_t* p =
                g_sprite.px + (static_cast<size_t>(y) * g_sprite.w + x) * 3;
            const int a = p[0] * global_a / 255;
            if (a <= 0) continue;
            const uint16_t src = static_cast<uint16_t>(p[1] | (p[2] << 8));
            uint16_t* dst = &fb.pixels[static_cast<size_t>(dy) * fb.width + dx];
            *dst = a >= 255 ? src : blend565(*dst, src, static_cast<uint8_t>(a));
        }
    }
}

// Meta mark approximation: a lemniscate of Bernoulli stroked as a
// disc walk, in Meta blue. Reads as the infinity loop at 20 px.
void draw_meta_mark(Framebuffer& fb, int cx, int cy, uint16_t color) {
    constexpr float kA = 22.0f;
    for (float t = 0.0f; t < 6.2832f; t += 0.05f) {
        const float s = std::sin(t);
        const float c = std::cos(t);
        const float d = 1.0f + s * s;
        const int x = static_cast<int>(cx + kA * c / d);
        const int y = static_cast<int>(cy + kA * s * c / d);
        fb.fill_circle(x, y, 3, color);
    }
}

}  // namespace

bool boot_anim_begin() {
    boot_anim_end();
    assets_load();   // ensures the FAT mount on device (no-op on host)
#ifdef __ESPRESSIF_IDF__
    const char* path = "/assets/boot_muse.bin";
#else
    char host_path[128];
    const char* dir = std::getenv("AIWATCHOS_ASSETS_DIR");
    if (dir && dir[0]) {
        std::snprintf(host_path, sizeof(host_path), "%s/boot_muse.bin", dir);
    } else {
        std::snprintf(host_path, sizeof(host_path), "assets/boot_muse.bin");
    }
    const char* path = host_path;
#endif
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 8) {
        std::fclose(f);
        return false;
    }
#ifdef __ESPRESSIF_IDF__
    uint8_t* buf = static_cast<uint8_t*>(
        heap_caps_malloc(static_cast<size_t>(size), MALLOC_CAP_SPIRAM));
    if (!buf) buf = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(size)));
#else
    uint8_t* buf = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(size)));
#endif
    if (!buf) {
        std::fclose(f);
        return false;
    }
    const size_t got = std::fread(buf, 1, static_cast<size_t>(size), f);
    std::fclose(f);
    if (got != static_cast<size_t>(size) || std::memcmp(buf, "AIBM", 4) != 0) {
        std::free(buf);
        return false;
    }
    const int w = buf[4] | (buf[5] << 8);
    const int h = buf[6] | (buf[7] << 8);
    if (w <= 0 || h <= 0 || w > kDisplayWidth ||
        size != 8L + static_cast<long>(w) * h * 3L) {
        std::free(buf);
        return false;
    }
    g_sprite_file = buf;
    g_sprite.w = w;
    g_sprite.h = h;
    g_sprite.px = buf + 8;
    return true;
}

void boot_anim_render(Framebuffer& fb, uint32_t t_ms) {
    if (t_ms > kBootAnimMs) t_ms = kBootAnimMs;   // past-the-end clamps
    fb.fill(0x0000);
    const int g_mark = ramp(t_ms, kMarkStart, kMarkLen);
    const int g_word = ramp(t_ms, kWordStart, kWordLen);
    const int g_meta = ramp(t_ms, kMetaStart, kMetaLen);
    const int g_credit = ramp(t_ms, kCreditStart, kCreditLen);

    // The mark: fade + 24 px rise, centred.
    if (g_mark > 0) {
        const int rise = 24 * (255 - g_mark) / 255;
        if (g_sprite.px) {
            draw_sprite(fb, (fb.width - g_sprite.w) / 2, 70 + rise, g_mark);
        } else {
            // Text-only fallback: a big "M" never strands the boot.
            draw_text_aa_centered(fb, fb.width / 2, 120 + rise, "M",
                                  FontFace::kDisplay,
                                  fade_to_black(rgb565_hex(0x1E87FF), g_mark));
        }
    }
    // Wordmark.
    if (g_word > 0) {
        const int rise = 12 * (255 - g_word) / 255;
        draw_text_aa_centered(fb, fb.width / 2, 300 + rise, "Muse",
                              FontFace::kTitle,
                              fade_to_black(colors::kWhite, g_word));
    }
    // Meta row: lemniscate mark + caption, centred as a unit.
    if (g_meta > 0) {
        constexpr int kMarkW = 54;
        const int cap_w = text_width_aa("Meta", FontFace::kCaption);
        const int total = kMarkW + 10 + cap_w;
        const int x0 = fb.width / 2 - total / 2;
        const int y0 = 384;
        draw_meta_mark(fb, x0 + kMarkW / 2, y0 + 10,
                       fade_to_black(rgb565_hex(0x0082FB), g_meta));
        draw_text_aa(fb, x0 + kMarkW + 10, y0 + 2, "Meta",
                     FontFace::kCaption,
                     fade_to_black(colors::kText, g_meta));
    }
    // Progress dots (start once the mark is underway).
    if (t_ms > 200) {
        const int phase = static_cast<int>((t_ms / 300) % 3);
        for (int i = 0; i < 3; ++i) {
            fb.fill_circle(fb.width / 2 - 17 + i * 17, 436, 4,
                           i == phase ? colors::kWhite : colors::kFaint);
        }
    }
    // Credit line.
    if (g_credit > 0) {
        draw_text_aa_centered(fb, fb.width / 2, 466, "AIWatchOS",
                              FontFace::kMicro,
                              fade_to_black(colors::kDim, g_credit));
    }
}

void boot_anim_end() {
    if (g_sprite_file) {
#ifdef __ESPRESSIF_IDF__
        heap_caps_free(g_sprite_file);
#else
        std::free(g_sprite_file);
#endif
        g_sprite_file = nullptr;
    }
    g_sprite = BootSprite{};
}

void boot_anim_play(Board& board, Framebuffer& fb) {
    boot_anim_begin();
    const uint64_t t0 = board.now_ms();
    int xs[2] = {0, 0};
    int ys[2] = {0, 0};
    for (;;) {
        const uint32_t t =
            static_cast<uint32_t>(board.now_ms() - t0);
        boot_anim_render(fb, t > kBootAnimMs ? kBootAnimMs : t);
        board.display_flush_rows(0, fb.height);
        if (t >= kBootAnimMs) break;
        if (board.read_touch(xs, ys) > 0) break;   // any touch skips
#ifdef __ESPRESSIF_IDF__
        vTaskDelay(pdMS_TO_TICKS(33));
#else
        break;   // host: one frame; the preview tool drives the rest
#endif
    }
    boot_anim_end();
}

}  // namespace aiwatchos
