// Framebuffer helper methods (defined in app.hpp).
#include "aiwatchos/app.hpp"
#include "aiwatchos/font5x7.hpp"
#include <cstring>

namespace aiwatchos {

void Framebuffer::fill(uint16_t color) {
    for (int i = 0; i < width * height; ++i) {
        pixels[i] = color;
    }
}

void Framebuffer::hline(int x0, int x1, int y, uint16_t color) {
    if (y < 0 || y >= height) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; ++x) {
        put_pixel(x, y, color);
    }
}

void Framebuffer::vline(int x, int y0, int y1, uint16_t color) {
    if (x < 0 || x >= width) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; ++y) {
        put_pixel(x, y, color);
    }
}

void Framebuffer::fill_rect(int x0, int y0, int x1, int y1, uint16_t color) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; ++y) {
        hline(x0, x1, y, color);
    }
}

// --- Shared bitmap text (5x7 font, font5x7.hpp) ---

namespace {
// Row bytes for one character: folds a-z to A-Z, returns the hollow box for
// printable characters outside the charset, and nullptr for control/non-ASCII
// bytes (drawn as blank but still advanced past by draw_text()).
const uint8_t* font_glyph(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c < 32 || c > 126) return nullptr;
    const uint8_t* glyph = kFont5x7[c - 32];
    bool empty = true;
    for (int r = 0; r < kFontGlyphH; ++r) {
        if (glyph[r]) { empty = false; break; }
    }
    if (empty && c != ' ') return kFontBoxGlyph;
    return glyph;
}
}  // namespace

void Framebuffer::draw_char(int x, int y, char c, uint16_t color, int scale) {
    if (scale < 1) scale = 1;
    const uint8_t* rows = font_glyph(c);
    if (!rows) return;
    for (int r = 0; r < kFontGlyphH; ++r) {
        for (int col = 0; col < kFontGlyphW; ++col) {
            if (rows[r] & (0x10 >> col)) {
                fill_rect(x + col * scale, y + r * scale,
                          x + col * scale + scale - 1,
                          y + r * scale + scale - 1, color);
            }
        }
    }
}

void Framebuffer::draw_text(int x, int y, const char* text, uint16_t color, int scale) {
    if (!text) return;
    if (scale < 1) scale = 1;
    int pen_x = x;
    for (const char* p = text; *p; ++p) {
        draw_char(pen_x, y, *p, color, scale);
        pen_x += kFontAdvance * scale;
    }
}

int Framebuffer::text_width(const char* text, int scale) {
    if (!text) return 0;
    if (scale < 1) scale = 1;
    size_t len = std::strlen(text);
    if (len == 0) return 0;
    return static_cast<int>(len) * kFontAdvance * scale - scale;   // no trailing gap
}

}  // namespace aiwatchos
