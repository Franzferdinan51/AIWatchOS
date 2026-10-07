// AIWatchOS application interface. Every app (muse, hermes, clock face,
// launcher, etc.) implements this struct's callbacks so the OS can manage its
// lifecycle uniformly. The OS calls these from the main task loop.
#pragma once

#include <cstdint>
#include <string>
#include <functional>

namespace aiwatchos {

// Touch event delivered to the foreground app. Coordinates are in screen space
// (x: 0..409, y: 0..501) after FT3168 mapping.
struct TouchEvent {
    enum Type { Press, Release, Move };
    int x;     // 0 .. width-1  (screen width = 410)
    int y;     // 0 .. height-1 (screen height = 502)
    Type type;
};

// An RGB565 framebuffer view the OS provides to apps for rendering. The buffer
// lives in PSRAM and is owned by the display driver. Apps write pixels into it;
// the OS flushes dirty regions after render() returns.
struct Framebuffer {
    uint16_t* pixels;   // row-major, RGB565 (byte-swapped big-endian for CO5300)
    int width;          // 410
    int height;         // 502

    inline void put_pixel(int x, int y, uint16_t color) {
        if (x >= 0 && x < width && y >= 0 && y < height) {
            pixels[y * width + x] = color;
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
};

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
