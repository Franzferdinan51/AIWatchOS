// Framebuffer helper methods (defined in app.hpp).
#include "aiwatchos/app.hpp"

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

}  // namespace aiwatchos
