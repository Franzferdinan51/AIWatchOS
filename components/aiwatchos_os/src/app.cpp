// Framebuffer helper methods (defined in app.hpp).
#include "aiwatchos/app.hpp"
#include "aiwatchos/font5x7.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace aiwatchos {

void Framebuffer::fill(uint16_t color) {
    // Perf (audit suspect #2): direct span writes — the old fill /
    // hline / fill_rect chain funnelled every pixel through the
    // bounds-checked put_pixel, which made even the static backdrop
    // cost ~206k checked writes per frame. These paths clip ONCE and
    // then write through a raw row pointer (same discipline as
    // fill_span below).
    const int n = width * height;
    for (int i = 0; i < n; ++i) {
        pixels[i] = color;
    }
}

void Framebuffer::hline(int x0, int x1, int y, uint16_t color) {
    if (y < 0 || y >= height) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x1 < 0 || x0 >= width) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= width) x1 = width - 1;
    uint16_t* row = pixels + y * width;
    for (int x = x0; x <= x1; ++x) {
        row[x] = color;
    }
}

void Framebuffer::vline(int x, int y0, int y1, uint16_t color) {
    if (x < 0 || x >= width) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y1 < 0 || y0 >= height) return;
    if (y0 < 0) y0 = 0;
    if (y1 >= height) y1 = height - 1;
    for (int y = y0; y <= y1; ++y) {
        pixels[y * width + x] = color;
    }
}

void Framebuffer::fill_rect(int x0, int y0, int x1, int y1, uint16_t color) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x1 < 0 || x0 >= width || y1 < 0 || y0 >= height) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= width) x1 = width - 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= height) y1 = height - 1;
    for (int y = y0; y <= y1; ++y) {
        uint16_t* row = pixels + y * width;
        for (int x = x0; x <= x1; ++x) {
            row[x] = color;
        }
    }
}

// --- Muse Gloss primitives -------------------------------------------------
// Span math ported from HermesGadget's hg::Canvas (firmware/core/src/
// canvas.cpp), the in-family reference implementation for this panel
// class, adapted to this framebuffer's inclusive fill_rect and extended
// with gradient/bubble composition from DESIGN_TOKENS section 4.

namespace {

// Fill [x0, x1) on row y with a direct pointer loop (clipped). The hot
// path for every span fill below — no per-pixel bounds checks.
inline void fill_span(uint16_t* px, int fb_w, int fb_h,
                      int x0, int x1, int y, uint16_t color) {
    if (y < 0 || y >= fb_h) return;
    if (x0 < 0) x0 = 0;
    if (x1 > fb_w) x1 = fb_w;
    if (x0 >= x1) return;
    uint16_t* row = px + y * fb_w;
    for (int x = x0; x < x1; ++x) row[x] = color;
}

// Corner inset for row i (0-based from the nearest horizontal edge) of a
// rounded rect with corner radius r — hg::Canvas::fill_round_rect's math.
inline int rr_inset(int r, int i) {
    int dy = r - i;
    int inset = 0;
    while (inset < r && (r - inset) * (r - inset) + dy * dy > r * r) ++inset;
    return inset;
}

// Largest dx >= 0 with dx*dx + dy*dy <= r*r (isqrt via float + fixup).
inline int circle_dx(int r, int dy) {
    int rem = r * r - dy * dy;
    if (rem < 0) return -1;
    int dx = static_cast<int>(std::sqrt(static_cast<double>(rem)));
    while ((dx + 1) * (dx + 1) <= rem) ++dx;
    while (dx * dx > rem) --dx;
    return dx;
}

}  // namespace

void Framebuffer::fill_round_rect(int x, int y, int w, int h, int r,
                                  uint16_t color) {
    if (w <= 0 || h <= 0) return;
    r = std::min(r, std::min(w, h) / 2);
    if (r <= 0) {
        for (int yy = y; yy < y + h; ++yy) {
            fill_span(pixels, width, height, x, x + w, yy, color);
        }
        return;
    }
    for (int yy = 0; yy < h; ++yy) {
        int inset = 0;
        if (yy < r) inset = rr_inset(r, yy);
        else if (yy >= h - r) inset = rr_inset(r, h - 1 - yy);
        fill_span(pixels, width, height, x + inset, x + w - inset, y + yy, color);
    }
}

void Framebuffer::stroke_round_rect(int x, int y, int w, int h, int r,
                                    uint16_t color, uint8_t alpha) {
    if (w <= 1 || h <= 1 || alpha == 0) return;
    r = std::min(r, std::min(w, h) / 2);
    auto blend_span = [&](int x0, int x1, int yy) {
        if (yy < 0 || yy >= height) return;
        if (x0 < 0) x0 = 0;
        if (x1 > width) x1 = width;
        for (int xx = x0; xx < x1; ++xx) put_pixel_blend(xx, yy, color, alpha);
    };
    for (int yy = 0; yy < h; ++yy) {
        int inset_o = 0;
        if (yy < r) inset_o = rr_inset(r, yy);
        else if (yy >= h - r) inset_o = rr_inset(r, h - 1 - yy);
        const int row_y = y + yy;
        if (yy == 0 || yy == h - 1) {
            blend_span(x + inset_o, x + w - inset_o, row_y);
            continue;
        }
        // Inner edge: the (w-2, h-2, r-1) round rect at (x+1, y+1).
        int inset_i;
        const int iy = yy - 1;
        const int ih = h - 2;
        const int ir = std::max(0, r - 1);
        if (iy < ir) inset_i = rr_inset(ir, iy) + 1;
        else if (iy >= ih - ir) inset_i = rr_inset(ir, ih - 1 - iy) + 1;
        else inset_i = 1;
        blend_span(x + inset_o, x + inset_i, row_y);            // left edge
        blend_span(x + w - inset_i, x + w - inset_o, row_y);    // right edge
    }
}

void Framebuffer::fill_circle(int cx, int cy, int r, uint16_t color) {
    if (r < 0) return;
    for (int dy = -r; dy <= r; ++dy) {
        int dx = circle_dx(r, dy);
        fill_span(pixels, width, height, cx - dx, cx + dx + 1, cy + dy, color);
    }
}

void Framebuffer::ring(int cx, int cy, int r, int thickness, uint16_t color) {
    if (r <= 0 || thickness <= 0) return;
    const int inner = std::max(0, r - thickness);
    for (int dy = -r; dy <= r; ++dy) {
        const int y = cy + dy;
        if (y < 0 || y >= height) continue;
        const int dxo = circle_dx(r, dy);
        if (dxo < 0) continue;
        if (inner == 0 || dy * dy > inner * inner) {
            fill_span(pixels, width, height, cx - dxo, cx + dxo + 1, y, color);
        } else {
            const int dxi = circle_dx(inner, dy);
            fill_span(pixels, width, height, cx - dxo, cx - dxi, y, color);
            fill_span(pixels, width, height, cx + dxi + 1, cx + dxo + 1, y, color);
        }
    }
}

void Framebuffer::arc(int cx, int cy, int r, int thickness,
                      float start_deg, float sweep_deg, uint16_t color) {
    if (r <= 0 || thickness <= 0 || sweep_deg <= 0) return;
    if (sweep_deg >= 360.0f) {
        ring(cx, cy, r, thickness, color);
        return;
    }
    if (sweep_deg > 180.0f) {   // split: the half-plane test needs <= 180
        arc(cx, cy, r, thickness, start_deg, 180.0f, color);
        arc(cx, cy, r, thickness, start_deg + 180.0f, sweep_deg - 180.0f, color);
        return;
    }
    const float kDeg2Rad = 3.14159265f / 180.0f;
    const float a0 = start_deg * kDeg2Rad;
    const float a1 = (start_deg + sweep_deg) * kDeg2Rad;
    const float d0x = std::cos(a0), d0y = std::sin(a0);
    const float d1x = std::cos(a1), d1y = std::sin(a1);
    const int inner = std::max(0, r - thickness);
    // A point p is inside the swept span iff it sits clockwise of the
    // start ray and counter-clockwise of the end ray (screen coords,
    // y down, angles clockwise from east): cross(d0, p) >= 0 and
    // cross(p, d1) >= 0. Per-pixel cost is two multiply-subtract pairs,
    // and only ring-band pixels are visited (run structure from ring()).
    for (int dy = -r; dy <= r; ++dy) {
        const int y = cy + dy;
        if (y < 0 || y >= height) continue;
        const int dxo = circle_dx(r, dy);
        if (dxo < 0) continue;
        int runs[2][2];
        int n_runs = 0;
        if (inner == 0 || dy * dy > inner * inner) {
            runs[n_runs][0] = -dxo; runs[n_runs][1] = dxo; ++n_runs;
        } else {
            const int dxi = circle_dx(inner, dy);
            runs[n_runs][0] = -dxo; runs[n_runs][1] = -dxi - 1; ++n_runs;
            runs[n_runs][0] = dxi + 1; runs[n_runs][1] = dxo; ++n_runs;
        }
        for (int ri = 0; ri < n_runs; ++ri) {
            for (int dx = runs[ri][0]; dx <= runs[ri][1]; ++dx) {
                const float px = static_cast<float>(dx);
                const float py = static_cast<float>(dy);
                // Epsilon absorbs float error in cos/sin at exact ray
                // angles (e.g. cos(270deg) ~ 1e-8, not 0): without it the
                // start-ray column itself fails the test by ~1e-6.
                if (d0x * py - d0y * px >= -1e-3f &&
                    px * d1y - py * d1x >= -1e-3f) {
                    put_pixel(cx + dx, y, color);
                }
            }
        }
    }
}

void Framebuffer::fill_vgrad(int x, int y, int w, int h,
                             uint16_t top, uint16_t mid, uint16_t bot) {
    if (w <= 0 || h <= 0) return;
    // Per 4-row band (DESIGN_TOKENS G1): one colour computation per band,
    // then solid row fills.
    for (int yy = 0; yy < h; yy += 4) {
        const uint16_t c = vgrad_color(top, mid, bot, yy, h);
        const int rows = std::min(4, h - yy);
        for (int i = 0; i < rows; ++i) {
            fill_span(pixels, width, height, x, x + w, y + yy + i, c);
        }
    }
}

void Framebuffer::fill_round_rect_vgrad(int x, int y, int w, int h, int r,
                                        uint16_t top, uint16_t mid,
                                        uint16_t bot) {
    if (w <= 0 || h <= 0) return;
    r = std::min(r, std::min(w, h) / 2);
    int band_start = 0;
    while (band_start < h) {
        const int band_end = std::min(h, band_start + 4);
        const uint16_t c = vgrad_color(top, mid, bot, band_start, h);
        for (int yy = band_start; yy < band_end; ++yy) {
            int inset = 0;
            if (r > 0) {
                if (yy < r) inset = rr_inset(r, yy);
                else if (yy >= h - r) inset = rr_inset(r, h - 1 - yy);
            }
            fill_span(pixels, width, height, x + inset, x + w - inset,
                      y + yy, c);
        }
        band_start = band_end;
    }
}

void Framebuffer::blit_alpha4(int x, int y, const AlphaSprite& sprite,
                              uint16_t color) {
    if (!sprite.alpha || sprite.w == 0 || sprite.h == 0) return;
    const int row_bytes = (sprite.w + 1) / 2;
    for (int sy = 0; sy < sprite.h; ++sy) {
        const int yy = y + sy;
        if (yy < 0 || yy >= height) continue;
        const uint8_t* row = sprite.alpha + sy * row_bytes;
        for (int sx = 0; sx < sprite.w; ++sx) {
            const uint8_t packed = row[sx >> 1];
            const int nib = (sx & 1) ? (packed & 0x0F) : (packed >> 4);
            if (nib == 0) continue;
            put_pixel_blend(x + sx, yy, color,
                            static_cast<uint8_t>(nib * 17));
        }
    }
}

void Framebuffer::blit_alpha4_vstretch(int x, int y,
                                       const AlphaSprite& sprite,
                                       uint16_t color, int target_h,
                                       int cap_top, int cap_bot) {
    if (!sprite.alpha || target_h <= 0) return;
    if (cap_top + cap_bot >= static_cast<int>(sprite.h) ||
        cap_top + cap_bot >= target_h) {
        blit_alpha4(x, y, sprite, color);   // nothing to stretch
        return;
    }
    const int row_bytes = (sprite.w + 1) / 2;
    const int src_mid = static_cast<int>(sprite.h) - cap_top - cap_bot;
    const int dst_mid = target_h - cap_top - cap_bot;
    for (int ty = 0; ty < target_h; ++ty) {
        int sy;
        if (ty < cap_top) sy = ty;
        else if (ty >= target_h - cap_bot) {
            sy = static_cast<int>(sprite.h) - (target_h - ty);
        } else {
            sy = cap_top + (ty - cap_top) * src_mid / dst_mid;
        }
        const int yy = y + ty;
        if (yy < 0 || yy >= height) continue;
        const uint8_t* row = sprite.alpha + sy * row_bytes;
        for (int sx = 0; sx < sprite.w; ++sx) {
            const uint8_t packed = row[sx >> 1];
            const int nib = (sx & 1) ? (packed & 0x0F) : (packed >> 4);
            if (nib == 0) continue;
            put_pixel_blend(x + sx, yy, color,
                            static_cast<uint8_t>(nib * 17));
        }
    }
}

void Framebuffer::draw_bubble(int x, int y, int w, int h, int r,
                              const BubbleStyle& style) {
    if (style.shadow && style.shadow->alpha) {
        blit_alpha4(x + style.shadow->anchor_x, y + style.shadow->anchor_y,
                    *style.shadow, style.shadow_color);
    }
    fill_round_rect_vgrad(x, y, w, h, r, style.top, style.mid, style.bot);
    // G2 gloss band: top 10 rows, white alpha 56 fading to 0, inset 12px
    // plus the corner inset so it stays inside the rounded top edge.
    const int gloss_rows = std::min(10, h - 1);
    for (int gy = 1; gy <= gloss_rows; ++gy) {
        int inset = 12;
        if (gy < r) inset += rr_inset(r, gy);
        const uint8_t a = static_cast<uint8_t>(56 * (10 - gy) / 10);
        if (a == 0) continue;
        const int yy = y + gy;
        if (yy < 0 || yy >= height) continue;
        int x0 = std::max(0, x + inset);
        int x1 = std::min(width, x + w - inset);
        for (int xx = x0; xx < x1; ++xx) {
            put_pixel_blend(xx, yy, colors::kWhite, a);
        }
    }
    // G3 inner highlight + border: 1px outline blended at the style alpha.
    if (style.border_alpha > 0) {
        stroke_round_rect(x, y, w, h, r, colors::kWhite, style.border_alpha);
    }
}

const BubbleStyle& bubble_style() {
    static const BubbleStyle s{colors::kBubbleTop, colors::kBubble,
                               colors::kBubbleBot, 56, nullptr,
                               colors::kBlue};
    return s;
}

const BubbleStyle& bubble_style_filled() {
    static const BubbleStyle s{colors::kBlueTop, colors::kBlue,
                               colors::kBlueBot, 87, nullptr,
                               colors::kBlue};
    return s;
}

std::vector<std::string> wrap_text(std::string_view text, int max_chars) {
    // Port of hg::wrap_text (HermesGadget firmware/core/src/canvas.cpp).
    std::vector<std::string> lines;
    if (max_chars < 1) max_chars = 1;
    size_t i = 0;
    while (i <= text.size()) {
        size_t nl = text.find('\n', i);
        std::string_view para =
            text.substr(i, nl == std::string_view::npos
                               ? std::string_view::npos
                               : nl - i);
        std::string line;
        size_t p = 0;
        while (p < para.size()) {
            while (p < para.size() && para[p] == ' ') ++p;
            size_t end = para.find(' ', p);
            if (end == std::string_view::npos) end = para.size();
            std::string_view word = para.substr(p, end - p);
            p = end;
            if (word.empty()) continue;
            while (static_cast<int>(word.size()) > max_chars) {
                if (!line.empty()) {
                    lines.push_back(line);
                    line.clear();
                }
                lines.emplace_back(word.substr(0, static_cast<size_t>(max_chars)));
                word.remove_prefix(static_cast<size_t>(max_chars));
            }
            if (word.empty()) continue;
            size_t needed = line.empty() ? word.size()
                                         : line.size() + 1 + word.size();
            if (static_cast<int>(needed) > max_chars) {
                lines.push_back(line);
                line.assign(word);
            } else {
                if (!line.empty()) line.push_back(' ');
                line.append(word);
            }
        }
        lines.push_back(line);
        if (nl == std::string_view::npos) break;
        i = nl + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
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
