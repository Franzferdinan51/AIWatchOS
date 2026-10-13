// AIWatchOS application interface. Every app (muse, hermes, clock face,
// launcher, etc.) implements this struct's callbacks so the OS can manage its
// lifecycle uniformly. The OS calls these from the main task loop.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <functional>

#include "aiwatchos/colors.hpp"

namespace aiwatchos {

// Touch event delivered to the foreground app. Coordinates are in screen space
// (x: 0..409, y: 0..501) after FT3168 mapping.
struct TouchEvent {
    enum Type { Press, Release, Move };
    int x;     // 0 .. width-1  (screen width = 410)
    int y;     // 0 .. height-1 (screen height = 502)
    Type type;
};

// A 4-bit alpha sprite (shadow shapes from tools/gen_font.py): coverage
// packed 2 px/byte, row-major, high nibble first. anchor_x/anchor_y give
// the sprite's top-left corner relative to the shape it belongs to
// (negative: the sprite extends past the shape by the blur margin).
struct AlphaSprite {
    uint16_t w = 0;
    uint16_t h = 0;
    int16_t anchor_x = 0;
    int16_t anchor_y = 0;
    const uint8_t* alpha = nullptr;   // (w*h + 1) / 2 bytes
};

// Bubble surface recipe (DESIGN_TOKENS section 4): G1 vertical 3-stop
// gradient clipped to a rounded rect + G2 gloss band + G3 1px border.
// `shadow` (optional) is blitted in `shadow_color` BEFORE the body, so
// the body covers the sprite's inner half; pass nullptr for no shadow.
struct BubbleStyle {
    uint16_t top;
    uint16_t mid;
    uint16_t bot;
    uint8_t border_alpha = 56;        // white border alpha (0.22 -> 56)
    const AlphaSprite* shadow = nullptr;
    uint16_t shadow_color = colors::kBlue;
};

// The standard surface bubble (dark) and filled (own/primary) bubble.
const BubbleStyle& bubble_style();
const BubbleStyle& bubble_style_filled();

// An RGB565 framebuffer view the OS provides to apps for rendering. The buffer
// lives in PSRAM and is owned by the display driver. Apps write pixels into it;
// the OS flushes dirty regions after render() returns.
//
// Pixel byte order: pixels are CANONICAL RGB565 in host order everywhere in
// the OS. The CO5300 panel wants big-endian RGB565 on the wire; that single
// byte-swap happens in the display flush (display_driver.cpp), never here.
struct Framebuffer {
    uint16_t* pixels;   // row-major, canonical RGB565 (see note above)
    int width;          // 410
    int height;         // 502

    inline void put_pixel(int x, int y, uint16_t color) {
        if (x >= 0 && x < width && y >= 0 && y < height) {
            pixels[y * width + x] = color;
        }
    }

    // Blend `color` over the existing pixel with 8-bit alpha (255 = opaque,
    // identical to put_pixel). Used by text AA, gloss bands and borders —
    // the ONLY per-pixel read-modify-write paths, all tightly bounded.
    inline void put_pixel_blend(int x, int y, uint16_t color, uint8_t alpha) {
        if (x >= 0 && x < width && y >= 0 && y < height) {
            uint16_t& dst = pixels[y * width + x];
            dst = blend565(dst, color, alpha);
        }
    }

    // Clear the entire framebuffer to a solid RGB565 colour.
    void fill(uint16_t color);

    // Draw a horizontal line (used by clock hands and UI elements).
    void hline(int x0, int x1, int y, uint16_t color);

    // Draw a vertical line.
    void vline(int x, int y0, int y1, uint16_t color);

    // Fill a rectangle (inclusive bounds).
    void fill_rect(int x0, int y0, int x1, int y1, uint16_t color);

    // --- Muse Gloss primitives (x/y/w/h form, DESIGN_TOKENS section 4) ---
    // All are span-based row fills (memset-class); blends are confined to
    // edges, gloss bands and text, never full-screen per-pixel passes.

    // Filled rounded rectangle: rect (x, y, w, h), corner radius r.
    void fill_round_rect(int x, int y, int w, int h, int r, uint16_t color);

    // 1px rounded-rect outline, blended over the current pixels.
    void stroke_round_rect(int x, int y, int w, int h, int r,
                           uint16_t color, uint8_t alpha);

    // Filled circle (span-based, port of hg::Canvas::fill_circle).
    void fill_circle(int cx, int cy, int r, uint16_t color);

    // Ring: disc of radius r minus disc of radius r-thickness (span-based).
    void ring(int cx, int cy, int r, int thickness, uint16_t color);

    // Arc: the part of a ring between start_deg and start_deg+sweep_deg.
    // Angles are in screen coordinates: 0 deg = east, increasing CLOCKWISE
    // (y grows downward), so 270 deg = top. Sweep may exceed 180 deg.
    void arc(int cx, int cy, int r, int thickness,
             float start_deg, float sweep_deg, uint16_t color);

    // Vertical 3-stop gradient (G1) over rect (x, y, w, h): computed per
    // 4-row band from vgrad_color() — no LUT storage, no per-pixel lerp.
    void fill_vgrad(int x, int y, int w, int h,
                    uint16_t top, uint16_t mid, uint16_t bot);

    // G1 gradient clipped to a rounded rect (per-row spans).
    void fill_round_rect_vgrad(int x, int y, int w, int h, int r,
                               uint16_t top, uint16_t mid, uint16_t bot);

    // The full bubble recipe: optional shadow blit, gradient body, G2
    // gloss band (top 10px, white fading 56 -> 0, inset 12px), G3 border.
    void draw_bubble(int x, int y, int w, int h, int r,
                     const BubbleStyle& style);

    // Blit a 4-bit alpha sprite, tinting covered pixels toward `color`.
    void blit_alpha4(int x, int y, const AlphaSprite& sprite, uint16_t color);

    // Like blit_alpha4 but stretches the sprite's vertical middle band
    // (rows [cap_top, h-cap_bot)) to cover target_h rows — used by the
    // card shadow, whose height varies with content.
    void blit_alpha4_vstretch(int x, int y, const AlphaSprite& sprite,
                              uint16_t color, int target_h,
                              int cap_top, int cap_bot);

    // Draw one character of the shared 5x7 bitmap font (font5x7.hpp) with its
    // top-left corner at (x, y). Each font pixel is a scale x scale block, so a
    // glyph occupies 5*scale x 7*scale pixels. Lowercase folds to uppercase;
    // characters outside the charset draw as a hollow box.
    // NOTE: retained for boot/debug use (AIWATCHOS_DEBUG_FONT); UI text uses
    // the AA renderer (font.hpp) since the Muse Gloss restyle.
    void draw_char(int x, int y, char c, uint16_t color, int scale = 2);

    // Draw a NUL-terminated string with draw_char(), advancing 6*scale pixels
    // per character (5 px glyph + 1 px spacing, in font pixels).
    void draw_text(int x, int y, const char* text, uint16_t color, int scale = 2);

    // Pixel width of a string at the given scale, excluding the trailing
    // inter-glyph gap. 0 for null/empty text. Use for centering/right-aligning.
    static int text_width(const char* text, int scale = 2);

    // Pixel height of one line of text at the given scale.
    static int text_height(int scale = 2) { return 7 * scale; }

    // Horizontal advance per character at the given scale.
    static int text_advance(int scale = 2) { return 6 * scale; }
};

// Greedy word wrap to `max_chars` columns (port of hg::wrap_text).
// Honours '\n'; breaks words longer than a line. Empty trailing lines
// are dropped.
std::vector<std::string> wrap_text(std::string_view text, int max_chars);

// The OS app interface. Each callback is optional except init() — apps provide
// only what they need. The OS dispatches events in this order:
//   1. init() once at boot when the app is first registered/launched.
//   2. tick(ms_elapsed) called every frame (~40 ms target, matching LVGL).
//   3. render(fb) called after tick; draw into the framebuffer here.
//   4. on_touch(event) delivered when a touch is detected while app is foreground.
struct App {
    const char* id;          // unique identifier (e.g. "muse", "hermes")

    void (*init)(void);                          // one-time setup
    void (*tick)(uint32_t elapsed_ms);           // per-frame update
    void (*render)(Framebuffer& fb);             // draw into framebuffer
    bool (*on_touch)(const TouchEvent& event);   // returns true if consumed

    // Optional lifecycle hooks (may be nullptr).
    void (*pause)(void);                          // app lost foreground
    void (*resume)(void);                         // app regained foreground
};

// Convenience: build an App with all callbacks set.
inline App make_app(const char* id,
                    void (*init_fn)(void) = nullptr,
                    void (*tick_fn)(uint32_t) = nullptr,
                    void (*render_fn)(Framebuffer&) = nullptr,
                    bool (*touch_fn)(const TouchEvent&) = nullptr,
                    void (*pause_fn)(void) = nullptr,
                    void (*resume_fn)(void) = nullptr) {
    return App{ id, init_fn, tick_fn, render_fn, touch_fn, pause_fn, resume_fn };
}

}  // namespace aiwatchos
