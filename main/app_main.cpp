// AIWatchOS main entry point — app_main() is the ESP-IDF equivalent of Arduino's
// setup()+loop(). It initializes hardware via HAL, registers all apps (clock face,
// launcher, muse voice, hermes agent), and starts the AppManager event loop.
#include "aiwatchos/app.hpp"
#include "aiwatchos/app_manager.hpp"
#include "aiwatchos/clock_face.hpp"
#include "aiwatchos/launcher_ui.hpp"
#include "aiwatchos/battery_display.hpp"
#include "aiwatchos/hal.hpp"
#include "aiwatchos/display_power.hpp"

#include "aiwatchos/settings_page.hpp"
#include "aiwatchos/notifications.hpp"
#include "aiwatchos/weather_app.hpp"

#include "aiwatchos_muse.hpp"
#include "aiwatchos_hermes.hpp"
#include <cstdio>

#ifdef __ESPRESSIF_IDF__
#include "aiwatchos_net/net.hpp"
#endif

#ifdef __ESPRESSIF_IDF__
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

extern "C" void app_main(void) {
    // --- 1. Hardware initialization via HAL ---
    aiwatchos::Board& board = aiwatchos::Board::instance();
    board.begin();   // CO5300 display, FT3168 touch, ES8311/ES7210 audio, AXP2101 PMU

    // Allocate the framebuffer in PSRAM (410 * 502 * 2 bytes = ~412 KB).
    // It must NOT be a plain static array: 412 KB far exceeds the ESP32-S3's
    // internal DRAM, so a static array would overflow DRAM at boot.
#ifdef __ESPRESSIF_IDF__
    uint16_t* s_framebuffer = static_cast<uint16_t*>(heap_caps_malloc(
        aiwatchos::kDisplayWidth * aiwatchos::kDisplayHeight * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM));
#else
    static uint16_t s_host_framebuffer[aiwatchos::kDisplayWidth * aiwatchos::kDisplayHeight];
    uint16_t* s_framebuffer = s_host_framebuffer;
#endif
    board.set_framebuffer(s_framebuffer);   // register the framebuffer with Board for display_flush()
#ifdef __ESPRESSIF_IDF__
    if (!s_framebuffer) {
        // PSRAM allocation failed (PSRAM not enabled in sdkconfig?). Idle with
        // periodic yields instead of dereferencing null and crashing.
        while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
#endif
    aiwatchos::Framebuffer fb{s_framebuffer, aiwatchos::kDisplayWidth, aiwatchos::kDisplayHeight};

    // --- 2. Create the AppManager and register apps ---
    static aiwatchos::AppManager g_app_mgr(board);

    // System UI: clock face (launched on boot) and launcher for app switching.
    aiwatchos::App clock_app = aiwatchos::ClockFace::make_app();
    g_app_mgr.register_app(&clock_app, true);   // launch the clock face on boot

    // Launcher app — wraps AppManager switch_next/switch_prev behind touch input.
    // Registered as a regular app so it can be switched to from the clock face.
    static aiwatchos::LauncherUI g_launcher(g_app_mgr);
    aiwatchos::App launcher_app = aiwatchos::make_app(
        "launcher",
        nullptr,  // init: no one-time setup needed (stateless UI over AppManager)
        nullptr,  // tick: the main loop handles touch dispatch directly
        [](aiwatchos::Framebuffer& fb) { g_launcher.render(fb); },
        [](const aiwatchos::TouchEvent& event) -> bool { return g_launcher.on_touch(event); }
    );
    g_app_mgr.register_app(&launcher_app);

    // AI voice agent apps — integrated from external repos. The adapter functions
    // (make_muse_app, make_hermes_app) return aiwatchos::App descriptors with all
    // callbacks set; no separate state variable is needed since the adapters manage
    // their own internal static state.
    aiwatchos::App muse_app = aiwatchos_muse::make_muse_app();
    g_app_mgr.register_app(&muse_app);

    aiwatchos::App hermes_app = aiwatchos_hermes::make_hermes_app();
    g_app_mgr.register_app(&hermes_app);

    // Smartwatch feature apps: settings, notifications, weather.
    aiwatchos::App settings_app = aiwatchos::SettingsPage::make_app();
    g_app_mgr.register_app(&settings_app);

    aiwatchos::App notif_app = aiwatchos::NotificationManager::make_app();
    g_app_mgr.register_app(&notif_app);

    aiwatchos::App weather_app = aiwatchos::WeatherApp::make_app();
    g_app_mgr.register_app(&weather_app);

    // --- 3. Status bar with battery display ---
    static aiwatchos::BatteryDisplay g_status_bar(board);

#ifdef __ESPRESSIF_IDF__
    // --- 3b. Network: WiFi + SNTP + the Muse link (components/aiwatchos_net).
    // Fails soft: with no credentials configured this leaves the watch in
    // today's offline outbox mode, untouched.
    aiwatchos_net::net_start();
#endif

    // --- 4. Main event loop (replaces Arduino's loop()) ---
    const uint32_t kFrameMs = 40;   // ~25 FPS, matching LVGL frame rate from board config

    // Display power policy (see aiwatchos/display_power.hpp): sleeps the
    // CO5300 after the configured inactivity timeout or on a PWR short
    // press, and wakes it on touch / BOOT / PWR. The timeout comes from
    // Kconfig (AIWATCHOS_DISPLAY_SLEEP_TIMEOUT_S, default 30 s) in firmware
    // builds; host builds use the same default.
#if defined(__ESPRESSIF_IDF__) && defined(CONFIG_AIWATCHOS_DISPLAY_SLEEP_TIMEOUT_S)
    const uint32_t kDisplaySleepTimeoutMs =
        static_cast<uint32_t>(CONFIG_AIWATCHOS_DISPLAY_SLEEP_TIMEOUT_S) * 1000u;
#else
    const uint32_t kDisplaySleepTimeoutMs = 30000;   // matches the Kconfig default
#endif
    static aiwatchos::DisplayPower g_power(kDisplaySleepTimeoutMs);

    while (true) {
        // Sample this frame's inputs first: the power policy needs the touch
        // points and button state before anything is dispatched or rendered.
        const uint64_t frame_now_ms = board.now_ms();
        int touch_xs[2] = {0}, touch_ys[2] = {0};
        int n_points = board.read_touch(touch_xs, touch_ys);
        const uint32_t buttons = board.read_buttons();

        // Display power transitions. On wake, execution falls through and
        // renders below, so the panel immediately shows the current frame;
        // on sleep, the render pass is skipped for this frame and beyond.
        const aiwatchos::DisplayPower::Action power_action =
            g_power.update(frame_now_ms, n_points, buttons);
        if (power_action == aiwatchos::DisplayPower::Action::kWake) {
            board.display_sleep(false);
        } else if (power_action == aiwatchos::DisplayPower::Action::kEnterSleep) {
            board.display_sleep(true);
        }

        if (g_power.asleep()) {
            // Panel asleep: input was still sampled above (that polling is
            // what makes wake instant — the FT3168 INT line keeps producing
            // touch reads while the panel sleeps), but tick/render/flush are
            // skipped entirely. No DMA traffic reaches the sleeping panel,
            // so there is no AMOLED burn-in and battery drain is minimal
            // while the watch sits on the bench.
#ifdef __ESPRESSIF_IDF__
            vTaskDelay(pdMS_TO_TICKS(kFrameMs));
#endif
            continue;
        }

        // Dispatch touch to the foreground app — unless this touch is the one
        // that woke the panel. DisplayPower swallows the waking press (and
        // the rest of it, until all fingers lift) so it cannot also fire the
        // UI element sitting under the waking finger.
        if (!g_power.swallow_touch()) {
            for (int i = 0; i < n_points; ++i) {
                aiwatchos::TouchEvent te{
                    touch_xs[i], touch_ys[i],
                    aiwatchos::TouchEvent::Press   // FT3168 reports press state via IRQ
                };
                // Dispatch to the foreground app first. If it doesn't handle the touch
                // (e.g., clock_touch returns false), fall back to edge-swipe navigation so
                // the user can always switch apps from any screen — this makes criterion 6
                // (touch-driven launcher UI) reachable at runtime regardless of which app is active.
                if (!g_app_mgr.on_touch(te)) {
                    g_launcher.on_touch(te);   // edge-swipe: left/right 30px triggers switch_prev/next
                }
            }
        }

        // Tick the foreground app (updates AI conversation state, voice pipeline).
        g_app_mgr.tick(kFrameMs);

        // Render: clear framebuffer, draw status bar + clock face/app content.
        fb.fill(0x0000);   // black background for AMOLED power saving

        // Status bar renders time and battery at the top of every screen.
        uint64_t now_ms = frame_now_ms;
        char time_str[16];
        unsigned hours = static_cast<unsigned>((now_ms / 3600000) % 24);
        unsigned minutes = static_cast<unsigned>((now_ms / 60000) % 60);
        snprintf(time_str, sizeof(time_str), "%u:%02u", hours, minutes);
        g_status_bar.render(fb, time_str);

#ifdef __ESPRESSIF_IDF__
        // Link-state strip directly under the status bar (36 px tall):
        // green = Muse linked, amber = connecting/retrying, dark grey =
        // offline/unconfigured. Drawn with the existing framebuffer
        // primitives only — no font involved.
        {
            uint16_t link_color = 0x2104;  // dark grey: off / no wifi
            switch (aiwatchos_net::muse_link_state()) {
                case aiwatchos_net::LinkState::kLinked:
                    link_color = 0x07E0;  // green
                    break;
                case aiwatchos_net::LinkState::kConnecting:
                case aiwatchos_net::LinkState::kWaitingWifi:
                case aiwatchos_net::LinkState::kBackoff:
                    link_color = 0xFEA0;  // amber
                    break;
                case aiwatchos_net::LinkState::kOff:
                    break;
            }
            fb.fill_rect(0, 36, aiwatchos::kDisplayWidth - 1, 38, link_color);
        }
#endif

        // App renders its content below the status bar.
        g_app_mgr.render(fb);

        // Flush the framebuffer to the CO5300 AMOLED panel via DMA.
        board.display_flush();

        // Delay for one frame (~40 ms). Without this yield the loop would
        // starve the IDLE task and trip the task watchdog.
#ifdef __ESPRESSIF_IDF__
        vTaskDelay(pdMS_TO_TICKS(kFrameMs));
#endif
    }
}
