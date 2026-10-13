// Baked-asset loading + AA text/icon rendering — see font.hpp.
#include "aiwatchos/font.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __ESPRESSIF_IDF__
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char* kFontTag = "aiw-font";
#endif

namespace aiwatchos {
namespace {

struct LoadedFont {
    const uint8_t* base = nullptr;      // whole file image (PSRAM)
    int glyph_count = 0;
    int ascent = 0;
    int descent = 0;
    int line_height = 0;
    const uint8_t* records = nullptr;   // base + 18, 14 B per glyph
    const uint8_t* blob = nullptr;      // coverage data
};

LoadedFont g_fonts[static_cast<int>(FontFace::kCount)];
AlphaSprite g_sprites[3];
int g_card_cap_top = 0;
int g_card_cap_bot = 0;
bool g_sprites_ok = false;
bool g_tried = false;
bool g_ready = false;

constexpr int kFaceCount = static_cast<int>(FontFace::kCount);
const char* kFaceFiles[kFaceCount] = {
    "font_micro15.bin", "font_caption16.bin", "font_body18.bin",
    "font_body18sb.bin", "font_headline20.bin", "font_title28.bin",
    "font_pair40.bin", "font_display96.bin", "font_icon20.bin",
    "font_icon40.bin", "font_wx64.bin",
};

inline uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
inline int16_t rdi16(const uint8_t* p) {
    return static_cast<int16_t>(rd16(p));
}

uint8_t* read_whole_file(const char* path, size_t* out_size) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return nullptr;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return nullptr;
    }
    uint8_t* buf = nullptr;
#ifdef __ESPRESSIF_IDF__
    buf = static_cast<uint8_t*>(
        heap_caps_malloc(static_cast<size_t>(size), MALLOC_CAP_SPIRAM));
#endif
    if (!buf) buf = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(size)));
    if (!buf) {
        std::fclose(f);
        return nullptr;
    }
    const size_t got = std::fread(buf, 1, static_cast<size_t>(size), f);
    std::fclose(f);
    if (got != static_cast<size_t>(size)) {
        std::free(buf);
        return nullptr;
    }
    *out_size = static_cast<size_t>(size);
    return buf;
}

bool parse_font(const uint8_t* buf, size_t size, LoadedFont& out) {
    if (size < 18 || std::memcmp(buf, "AIF1", 4) != 0) return false;
    if (rd16(buf + 4) != 1) return false;
    const int count = rd16(buf + 6);
    const uint32_t data_bytes = rd32(buf + 14);
    if (18 + static_cast<size_t>(count) * 14 + data_bytes > size) return false;
    out.base = buf;
    out.glyph_count = count;
    out.ascent = rdi16(buf + 8);
    out.descent = rdi16(buf + 10);
    out.line_height = rd16(buf + 12);
    out.records = buf + 18;
    out.blob = buf + 18 + count * 14;
    return true;
}

bool parse_sprites(const uint8_t* buf, size_t size) {
    if (size < 8 || std::memcmp(buf, "AIS1", 4) != 0) return false;
    const int count = rd16(buf + 6);
    if (count != 3) return false;
    const uint8_t* rec = buf + 8;
    const uint8_t* blob = buf + 8 + count * 16;
    size_t offset = 0;
    for (int i = 0; i < count; ++i, rec += 16) {
        const uint32_t bytes = rd32(rec + 12);
        if (static_cast<size_t>(blob - buf) + offset + bytes > size) {
            return false;
        }
        g_sprites[i].w = rd16(rec);
        g_sprites[i].h = rd16(rec + 2);
        g_sprites[i].anchor_x = rdi16(rec + 4);
        g_sprites[i].anchor_y = rdi16(rec + 6);
        g_sprites[i].alpha = blob + offset;
        if (i == 2) {
            g_card_cap_top = rd16(rec + 8);
            g_card_cap_bot = rd16(rec + 10);
        }
        offset += bytes;
    }
    return true;
}

const uint8_t* find_glyph(const LoadedFont& f, uint32_t cp) {
    for (int i = 0; i < f.glyph_count; ++i) {
        const uint8_t* r = f.records + i * 14;
        if (rd32(r) == cp) return r;
    }
    return nullptr;
}

uint32_t decode_utf8(const char*& p, const char* end) {
    const auto c0 = static_cast<uint8_t>(*p);
    if (c0 < 0x80) {
        ++p;
        return c0;
    }
    int extra = (c0 & 0xE0) == 0xC0 ? 1 : (c0 & 0xF0) == 0xE0 ? 2
                : (c0 & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0 || p + extra >= end) {
        ++p;   // invalid lead byte: skip one byte
        return 0xFFFD;
    }
    uint32_t cp = c0 & (extra == 1 ? 0x1F : extra == 2 ? 0x0F : 0x07);
    for (int i = 1; i <= extra; ++i) {
        const auto ci = static_cast<uint8_t>(p[i]);
        if ((ci & 0xC0) != 0x80) {
            ++p;
            return 0xFFFD;
        }
        cp = (cp << 6) | (ci & 0x3F);
    }
    p += extra + 1;
    return cp;
}

// 5x7 fallback geometry per face (degraded mode only).
int fallback_scale(FontFace face) {
    switch (face) {
        case FontFace::kMicro: return 2;
        case FontFace::kCaption: return 2;
        case FontFace::kBody: return 2;
        case FontFace::kBodySemibold: return 2;
        case FontFace::kHeadline: return 3;
        case FontFace::kTitle: return 4;
        case FontFace::kPair: return 6;
        case FontFace::kDisplay: return 8;
        default: return 2;
    }
}

int icon_em(FontFace face) {
    switch (face) {
        case FontFace::kIcon20: return 20;
        case FontFace::kIcon40: return 40;
        case FontFace::kIconWx64: return 64;
        default: return 20;
    }
}

void draw_glyph(Framebuffer& fb, const LoadedFont& f, const uint8_t* rec,
                int pen_x, int baseline_y, uint16_t color) {
    const int w = rec[4];
    const int h = rec[5];
    if (w == 0 || h == 0) return;
    const int xoff = static_cast<int8_t>(rec[6]);
    const int yoff = static_cast<int8_t>(rec[7]);
    const uint8_t* data = f.blob + rd32(rec + 10);
    const int stride = (w + 1) / 2;
    for (int gy = 0; gy < h; ++gy) {
        const int yy = baseline_y + yoff + gy;
        if (yy < 0 || yy >= fb.height) continue;
        const uint8_t* row = data + gy * stride;
        for (int gx = 0; gx < w; ++gx) {
            const uint8_t packed = row[gx >> 1];
            const int nib = (gx & 1) ? (packed & 0x0F) : (packed >> 4);
            if (nib == 0) continue;
            fb.put_pixel_blend(pen_x + xoff + gx, yy, color,
                               static_cast<uint8_t>(nib * 17));
        }
    }
}

void ensure_loaded() {
    if (!g_tried) assets_load();
}

}  // namespace

bool assets_load() {
    if (g_tried) return g_ready;
    g_tried = true;
#ifdef __ESPRESSIF_IDF__
    // Mount the assets partition read-only (image baked at build time
    // by fatfs_create_spiflash_image in the root CMakeLists). A failed
    // mount is not fatal: the file opens below simply fail and the UI
    // runs in the 5x7 fallback.
    static const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 4096,
    };
    const esp_err_t mount_err = esp_vfs_fat_spiflash_mount_ro(
        "/assets", "aiwatchos_assets", &mount_config);
    // This result used to be discarded, which is why the first-boot
    // log (2026-10-13) contained ZERO font/FAT lines while the watch
    // rendered fallback blocks: nobody could tell mount failure from
    // missing files from parse failure. Log all three stages now.
    if (mount_err == ESP_OK) {
        ESP_LOGI(kFontTag, "assets partition mounted at /assets");
    } else {
        ESP_LOGW(kFontTag, "assets mount FAILED: %s",
                 esp_err_to_name(mount_err));
    }
    const char* prefix = "/assets/";
#else
    const char* prefix = "assets/";
    if (const char* dir = std::getenv("AIWATCHOS_ASSETS_DIR")) {
        (void)dir;   // handled per-file below via the same prefix logic
    }
#endif
    bool all = true;
    int faces_ok = 0;
    for (int i = 0; i < kFaceCount; ++i) {
        char path[96];
#ifdef __ESPRESSIF_IDF__
        std::snprintf(path, sizeof(path), "%s%s", prefix, kFaceFiles[i]);
#else
        const char* dir = std::getenv("AIWATCHOS_ASSETS_DIR");
        if (dir && dir[0]) {
            std::snprintf(path, sizeof(path), "%s/%s", dir, kFaceFiles[i]);
        } else {
            std::snprintf(path, sizeof(path), "%s%s", prefix, kFaceFiles[i]);
        }
#endif
        size_t size = 0;
        uint8_t* buf = read_whole_file(path, &size);
        if (!buf || !parse_font(buf, size, g_fonts[i])) {
            all = false;   // face stays unavailable; others may still load
#ifdef __ESPRESSIF_IDF__
            ESP_LOGW(kFontTag, "font face unavailable: %s (%s)", path,
                     buf ? "bad AIF1 image" : "open failed");
#endif
        } else {
            ++faces_ok;
        }
    }
#ifdef __ESPRESSIF_IDF__
    ESP_LOGI(kFontTag, "assets load: %d/%d font faces, sprites %s",
             faces_ok, kFaceCount, g_sprites_ok ? "ok" : "unavailable");
#endif
    {
        char path[96];
#ifdef __ESPRESSIF_IDF__
        std::snprintf(path, sizeof(path), "%ssprites.bin", prefix);
#else
        const char* dir = std::getenv("AIWATCHOS_ASSETS_DIR");
        if (dir && dir[0]) {
            std::snprintf(path, sizeof(path), "%s/sprites.bin", dir);
        } else {
            std::snprintf(path, sizeof(path), "%ssprites.bin", prefix);
        }
#endif
        size_t size = 0;
        uint8_t* buf = read_whole_file(path, &size);
        if (buf) g_sprites_ok = parse_sprites(buf, size);
    }
    g_ready = all;
    return g_ready;
}

bool assets_ready() {
    ensure_loaded();
    return g_ready;
}

bool font_available(FontFace face) {
    ensure_loaded();
    return g_fonts[static_cast<int>(face)].base != nullptr;
}

int font_ascent(FontFace face) {
    ensure_loaded();
    const LoadedFont& f = g_fonts[static_cast<int>(face)];
    if (f.base) return f.ascent;
    return 7 * fallback_scale(face);
}

int font_line_height(FontFace face) {
    ensure_loaded();
    const LoadedFont& f = g_fonts[static_cast<int>(face)];
    if (f.base) return f.line_height;
    return 7 * fallback_scale(face);
}

int text_width_aa(std::string_view utf8, FontFace face) {
    ensure_loaded();
    const LoadedFont& f = g_fonts[static_cast<int>(face)];
    if (!f.base) {
        // 5x7 fallback width on the ASCII-folded text.
        int n = 0;
        for (char c : utf8) {
            if ((static_cast<uint8_t>(c) & 0xC0) != 0x80) ++n;
        }
        if (n == 0) return 0;
        return n * Framebuffer::text_advance(fallback_scale(face)) -
               fallback_scale(face);
    }
    int width = 0;
    const char* p = utf8.data();
    const char* end = p + utf8.size();
    while (p < end) {
        const uint32_t cp = decode_utf8(p, end);
        const uint8_t* rec = find_glyph(f, cp);
        if (rec) width += rec[8];
    }
    return width;
}

void draw_text_aa(Framebuffer& fb, int x, int y, std::string_view utf8,
                  FontFace face, uint16_t color) {
    ensure_loaded();
    const LoadedFont& f = g_fonts[static_cast<int>(face)];
    if (!f.base) {
        // 5x7 fallback: fold UTF-8 to ASCII bytes ('?' for the rest) so
        // the degraded path still shows the gist.
        char buf[192];
        size_t n = 0;
        const char* p = utf8.data();
        const char* end = p + utf8.size();
        while (p < end && n + 1 < sizeof(buf)) {
            const uint32_t cp = decode_utf8(p, end);
            buf[n++] = (cp < 128) ? static_cast<char>(cp) : '?';
        }
        buf[n] = '\0';
        fb.draw_text(x, y, buf, color, fallback_scale(face));
        return;
    }
    const int baseline = y + f.ascent;
    int pen = x;
    const char* p = utf8.data();
    const char* end = p + utf8.size();
    while (p < end) {
        const uint32_t cp = decode_utf8(p, end);
        const uint8_t* rec = find_glyph(f, cp);
        if (!rec) continue;
        draw_glyph(fb, f, rec, pen, baseline, color);
        pen += rec[8];
    }
}

void draw_text_aa_centered(Framebuffer& fb, int cx, int y,
                           std::string_view utf8, FontFace face,
                           uint16_t color) {
    draw_text_aa(fb, cx - text_width_aa(utf8, face) / 2, y, utf8, face,
                 color);
}

void draw_text_aa_right(Framebuffer& fb, int x_right, int y,
                        std::string_view utf8, FontFace face,
                        uint16_t color) {
    draw_text_aa(fb, x_right - text_width_aa(utf8, face), y, utf8, face,
                 color);
}

void draw_icon(Framebuffer& fb, int x, int y, Icon icon, FontFace face,
               uint16_t color) {
    ensure_loaded();
    const LoadedFont& f = g_fonts[static_cast<int>(face)];
    if (!f.base) {
        const int em = icon_em(face);
        fb.stroke_round_rect(x + 2, y + 2, em - 4, em - 4, 4, color, 128);
        return;
    }
    const uint8_t* rec = find_glyph(f, static_cast<uint32_t>(icon));
    if (!rec) return;
    // Icon glyphs are baked with the baseline at the em bottom, so the
    // ink lands in the em box whose top-left is (x, y).
    draw_glyph(fb, f, rec, x, y + f.ascent, color);
}

const AlphaSprite* shadow_tile() {
    ensure_loaded();
    return g_sprites_ok ? &g_sprites[0] : nullptr;
}

const AlphaSprite* shadow_pill() {
    ensure_loaded();
    return g_sprites_ok ? &g_sprites[1] : nullptr;
}

const AlphaSprite* shadow_card() {
    ensure_loaded();
    return g_sprites_ok ? &g_sprites[2] : nullptr;
}

void card_shadow_caps(int& cap_top, int& cap_bot) {
    ensure_loaded();
    cap_top = g_card_cap_top;
    cap_bot = g_card_cap_bot;
}

}  // namespace aiwatchos
