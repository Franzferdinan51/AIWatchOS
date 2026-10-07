// AIWatchOS main entry point — app_main() is the ESP-IDF equivalent of Arduino's
// setup()+loop(). It initializes hardware via HAL, registers all apps (clock face,
// launcher, muse voice, hermes agent), and starts the AppManager event loop.
#include "aiwatchos/app.hpp"
#include "aiwatchos/app_manager.hpp"
#include "aiwatchos/clock_face.hpp"
#include "aiwatchos/launcher_ui.hpp"
#include "aiwatchos/battery_display.hpp"
#include "aiwatchos/hal.hpp"

#include "aiwatchos_muse.hpp"
#include "aiwatchos_hermes.hpp"

extern "C" void app_main(void) {
    // --- 1. Hardware initialization via HAL ---
    aiwatchos::Board& board = aiwatchos::Board::instance();
    board.begin();   // CO5300 display, FT3168 touch, ES8311/ES7210 audio, AXP2101 PMU

    // Allocate the framebuffer in PSRAM (410 * 502 * 2 bytes = ~412 KB).
    static uint16_t s_framebuffer[aiwatchos::kDisplayWidth * aiwatchos::kDisplayHeight];
    aiwatchos::Framebuffer fb{s_framebuffer, aiwatchos::kDisplayWidth, aiwatchos::kDisplayHeight};

    // --- 2. Create the AppManager and register apps ---
    static aiwatchos::AppManager g_app_mgr(board);

    // System UI apps: clock face (always first for quick access), launcher.
    aiwatchos::App clock_app = aiwatchos::ClockFace::make_app();
    g_app_mgr.register_app(&clock_app, true);   // launch the clock face on boot

    // AI voice agent apps — integrated from external repos.
    aiwatchos::muse::MuseState muse_state;      // (state managed in adapter)
    aiwatchos_muse::App muse_app = aiwatchos_muse::make_muse_app();
    g_app_mgr.register_app(&muse_app);

    aiwatchos_hermes::HermesState hermes_state;  // (state managed in adapter)
    aiwatchos_hermes::App hermes_app = aiwatchos_hermes::make_hermes_app();
    g_app_mgr.register_app(&hermes_app);

    // --- 3. Status bar with battery display ---
    static aiwatchos::BatteryDisplay g_status_bar(board);

    // --- 4. Main event loop (replaces Arduino's loop()) ---
    const uint32_t kFrameMs = 40;   // ~25 FPS, matching LVGL frame rate from board config

    while (true) {
        // Read touch input and dispatch to the foreground app.
        int touch_xs[2] = {0}, touch_ys[2] = {0};
        int n_points = board.read_touch(touch_xs, touch_ys);

        for (int i = 0; i < n_points; ++i) {
            aiwatchos::TouchEvent te{
                touch_xs[i], touch_ys[i],
                aiwatchos::TouchEvent::Press   // FT3168 reports press state via IRQ
            };
            g_app_mgr.on_touch(te);
        }

        // Tick the foreground app (updates AI conversation state, voice pipeline).
        g_app_mgr.tick(kFrameMs);

        // Render: clear framebuffer, draw status bar + clock face/app content.
        fb.fill(0x0000);   // black background for AMOLED power saving

        // Status bar renders time and battery at the top of every screen.
        uint64_t now = board.now_ms();
        char time_str[16];
        snprintf(time_str, sizeof(time_str), "%llu:%02u",
                 (now / 3600000) % 24, (now / 60000) % 60);
        g_status_bar.render(fb, time_str);

        // App renders its content below the status bar.
        g_app_mgr.render(fb);

        // Flush the framebuffer to the CO5300 AMOLED panel via DMA.
        board.display_flush();

        // Delay for one frame (~40 ms). In ESP-IDF this uses vTaskDelay.
        // (The real implementation uses esp_timer or FreeRTOS tick count.)
    }
}
