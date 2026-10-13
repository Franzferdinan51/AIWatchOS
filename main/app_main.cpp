// AIWatchOS main entry point — app_main() is the ESP-IDF equivalent of Arduino's
// setup()+loop(). It initializes hardware via HAL, registers all apps (clock face,
// launcher, muse voice, hermes agent), and starts the AppManager event loop.
#include "aiwatchos/app.hpp"
#include "aiwatchos/app_manager.hpp"
#include "aiwatchos/band_flush.hpp"
#include "aiwatchos/font.hpp"
#include "aiwatchos/touch_synth.hpp"
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
#include <cstring>

#ifdef __ESPRESSIF_IDF__
#include "aiwatchos_net/net.hpp"
#include "aiwatchos_weather/weather_service.hpp"
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

    // --- 3c. Weather: NWS (api.weather.gov) fetch task; publishes real readings
    // into the weather app via its provider seam. Fails soft the same
    // way: unconfigured WiFi leaves the app in its NO DATA YET state.
    aiwatchos_weather::weather_service_start();
#endif

    // Load the baked font/sprite assets into PSRAM (mounted from the
    // aiwatchos_assets FAT partition on device). Failure leaves the UI
    // in the 5x7 fallback — degraded, never textless.
    aiwatchos::assets_load();

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

    // Touch event synthesis (Press/Move/Release from the FT3168 point
    // stream) and band-based dirty flushing (plan 1.6/1.7).
    static aiwatchos::TouchSynth g_touch_synth;
    static aiwatchos::BandFlush g_band_flush;
    // App-switch brightness dip state (plan 3.0 transition).
    const char* last_app_id = nullptr;
    int dip_step = -1;
    uint8_t dip_saved = 100;

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
            g_band_flush.invalidate();   // first awake frame flushes fully
            g_touch_synth.reset();       // no phantom events across the wake
        } else if (power_action == aiwatchos::DisplayPower::Action::kEnterSleep) {
            board.display_sleep(true);
            g_touch_synth.reset();
        }

        // Synthesize Press/Move/Release from the raw point stream (audit
        // X6). Runs before the asleep check so a finger held through
        // sleep/wake stays consistent; dispatch is gated below.
        aiwatchos::TouchEvent synth_events[aiwatchos::TouchSynth::kMaxEvents];
        const int n_events = g_touch_synth.update(n_points, touch_xs, touch_ys,
                                                  synth_events);

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
            for (int i = 0; i < n_events; ++i) {
                const aiwatchos::TouchEvent& te = synth_events[i];
                // Foreground app first. While the launcher is foreground
                // it receives the events itself (tiles + its own swipes).
                if (g_app_mgr.on_touch(te)) continue;
                // Fallback (plan 3.0): an unconsumed Press in the bottom
                // 30 px strip — below every app's controls — returns
                // home. This replaces the old left/right edge-swipe
                // fallback (7.5%-of-width edge targets on a 410 px screen
                // fired accidentally mid-gesture); the bottom strip is
                // one large thumb target instead.
                if (te.type == aiwatchos::TouchEvent::Press &&
                    te.y >= aiwatchos::kDisplayHeight - 30) {
                    g_app_mgr.switch_to("launcher");
                }
            }
        }

        // Tick the foreground app (updates AI conversation state, voice pipeline).
        g_app_mgr.tick(kFrameMs);

        // App-switch dip (plan 3.0): when the foreground app changes,
        // brightness dips toward black and back over ~6 frames, so the
        // user never sees a full-brightness cut between two screens.
        {
            const char* current_id = g_app_mgr.current_app_id();
            if (dip_step < 0 && last_app_id && current_id &&
                std::strcmp(last_app_id, current_id) != 0) {
                dip_saved = board.backlight_percent();
                dip_step = 0;
            }
            if (current_id) last_app_id = current_id;
            if (dip_step >= 0) {
                static const uint8_t kDipPct[6] = {45, 15, 0, 15, 45, 100};
                board.set_backlight(static_cast<uint8_t>(
                    dip_saved * kDipPct[dip_step] / 100));
                if (++dip_step >= 6) dip_step = -1;
            }
        }

        // Render (plan 3.0 order): G5 backdrop -> app -> shell status
        // zone LAST, so the status bar is present and identical on
        // every screen and no app can paint over it (audit X3).
        aiwatchos::draw_shell_backdrop(fb);
        g_app_mgr.render(fb);

        uint64_t now_ms = aiwatchos::local_display_ms(frame_now_ms);  // display-only TZ shift
        char time_str[16];
        unsigned hours = static_cast<unsigned>((now_ms / 3600000) % 24);
        unsigned minutes = static_cast<unsigned>((now_ms / 60000) % 60);
        snprintf(time_str, sizeof(time_str), "%u:%02u", hours, minutes);
        g_status_bar.render(fb, time_str);

#ifdef __ESPRESSIF_IDF__
        // Link-state strip: the bottom 3px INSIDE the status zone
        // (tokens section 8 position), state-coloured: ok = linked,
        // warn = connecting/retrying, faint = off/unconfigured.
        {
            uint16_t link_color = aiwatchos::colors::kFaint;
            switch (aiwatchos_net::muse_link_state()) {
                case aiwatchos_net::LinkState::kLinked:
                    link_color = aiwatchos::colors::kOk;
                    break;
                case aiwatchos_net::LinkState::kConnecting:
                case aiwatchos_net::LinkState::kWaitingWifi:
                case aiwatchos_net::LinkState::kBackoff:
                    link_color = aiwatchos::colors::kWarn;
                    break;
                case aiwatchos_net::LinkState::kOff:
                    break;
            }
            fb.fill_rect(0, aiwatchos::kStatusZoneH - 3,
                         aiwatchos::kDisplayWidth - 1,
                         aiwatchos::kStatusZoneH - 1, link_color);
        }
#endif

        // Flush only the changed bands to the CO5300 (plan 1.6): hash
        // the frame in 4 horizontal bands and flush the dirty ones via
        // the HAL row-range flush. A static screen costs zero QSPI
        // traffic; changed bands commit their hashes only on success.
        {
            const uint8_t dirty = g_band_flush.scan(fb.pixels, fb.width, fb.height);
            uint8_t flushed = 0;
            for (int b = 0; b < aiwatchos::BandFlush::kBands; ++b) {
                if (!(dirty & (1u << b))) continue;
                int y0, y1;
                aiwatchos::BandFlush::band_range(b, fb.height, y0, y1);
                if (board.display_flush_rows(y0, y1)) {
                    flushed |= static_cast<uint8_t>(1u << b);
                }
            }
            g_band_flush.commit(flushed);
        }

        // Delay for one frame (~40 ms). Without this yield the loop would
        // starve the IDLE task and trip the task watchdog.
#ifdef __ESPRESSIF_IDF__
        vTaskDelay(pdMS_TO_TICKS(kFrameMs));
#endif
    }
}
