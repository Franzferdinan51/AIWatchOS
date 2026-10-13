// AIWatchOS main entry point — app_main() is the ESP-IDF equivalent of Arduino's
// setup()+loop(). It initializes hardware via HAL, registers all apps (clock face,
// launcher, muse voice, hermes agent), and starts the AppManager event loop.
#include "aiwatchos/app.hpp"
#include "aiwatchos/app_manager.hpp"
#include "mdb.hpp"
#include "aiwatchos/band_flush.hpp"
#include "aiwatchos/font.hpp"
#include "aiwatchos/touch_synth.hpp"
#include "aiwatchos/clock_face.hpp"
#include "aiwatchos/launcher_ui.hpp"
#include "aiwatchos/battery_display.hpp"
#include "aiwatchos/colors.hpp"
#include "aiwatchos/font5x7.hpp"
#include "aiwatchos/hal.hpp"
#include "aiwatchos/display_power.hpp"
#include "aiwatchos/boot_escape.hpp"

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


// ---------------------------------------------------------------------------
// MDB control hooks (docs/mdb.md). The console task fills this state;
// the main loop consumes it at its normal input sampling point via
// aiwatchos_mdb::detail::apply_frame(), so injected input travels the
// exact path physical input travels (DisplayPower -> TouchSynth ->
// AppManager). Single producer (console task) / single consumer (main
// loop) ring; the state is debug-grade by design.
// ---------------------------------------------------------------------------
namespace aiwatchos_mdb {
namespace {

struct InjFrame {
    int n = 0;
    int x = 0;
    int y = 0;
};
InjFrame g_inj_frames[48];
volatile int g_inj_head = 0;   // consumer position
volatile int g_inj_tail = 0;   // producer position
volatile uint32_t g_inj_button_mask = 0;
volatile int g_inj_button_frames = 0;
char g_inj_switch[24] = {};
volatile bool g_inj_switch_pending = false;
volatile bool g_frame_card = false;


void inj_push(int n, int x, int y) {
    const int next = (g_inj_tail + 1) % 48;
    if (next == g_inj_head) return;   // full: drop (debug input)
    g_inj_frames[g_inj_tail].n = n;
    g_inj_frames[g_inj_tail].x = x;
    g_inj_frames[g_inj_tail].y = y;
    g_inj_tail = next;
}

}  // namespace

namespace detail {
aiwatchos::DisplayPower* g_power_hook = nullptr;
aiwatchos::AppManager* g_apps_hook = nullptr;
}  // namespace detail

void inject_tap(int x, int y) {
    // ~120 ms down (3 frames at the 40 ms loop cadence), then a
    // release frame so TouchSynth emits Press + Release.
    for (int i = 0; i < 3; ++i) inj_push(1, x, y);
    inj_push(0, 0, 0);
}

void inject_swipe(int x1, int y1, int x2, int y2, int duration_ms) {
    int steps = duration_ms / 40;
    if (steps < 2) steps = 2;
    if (steps > 24) steps = 24;
    for (int i = 0; i < steps; ++i) {
        const int x = x1 + (x2 - x1) * i / (steps - 1);
        const int y = y1 + (y2 - y1) * i / (steps - 1);
        inj_push(1, x, y);
    }
    inj_push(0, 0, 0);
}

void inject_button(uint32_t mask) {
    // 5 frames ~ 200 ms: inside DisplayPower's short-press window
    // (30..1000 ms) for PWR, a clean tap for BOOT.
    g_inj_button_mask = mask;
    g_inj_button_frames = 5;
}

void request_switch_app(const char* app_id) {
    if (!app_id) return;
    std::strncpy(g_inj_switch, app_id, sizeof(g_inj_switch) - 1);
    g_inj_switch[sizeof(g_inj_switch) - 1] = '\0';
    g_inj_switch_pending = true;
}

void request_wake() {
    // A touch wakes the panel, and DisplayPower swallows the waking
    // press until release — a centre tap therefore cannot hit any UI.
    if (display_asleep()) {
        inject_tap(aiwatchos::kDisplayWidth / 2, aiwatchos::kDisplayHeight / 2);
    }
}

void request_sleep() {
    // PWR short press toggles sleep on release (DisplayPower policy).
    if (detail::g_power_hook && !detail::g_power_hook->asleep()) inject_button(0x02);
}

void request_frame_card(bool on) { g_frame_card = on; }

bool display_asleep() {
    return detail::g_power_hook && detail::g_power_hook->asleep();
}

const char* current_app_id() {
    return detail::g_apps_hook ? detail::g_apps_hook->current_app_id()
                               : nullptr;
}

namespace detail {
void apply_frame(int* n_points, int* xs, int* ys, uint32_t* buttons) {
    if (g_inj_switch_pending) {
        g_inj_switch_pending = false;
        if (g_apps_hook) g_apps_hook->switch_to(g_inj_switch);
    }
    if (g_inj_head != g_inj_tail) {
        const InjFrame& f = g_inj_frames[g_inj_head];
        g_inj_head = (g_inj_head + 1) % 48;
        *n_points = f.n;
        if (f.n > 0) {
            xs[0] = f.x;
            ys[0] = f.y;
        }
    }
    if (g_inj_button_frames > 0) {
        *buttons |= g_inj_button_mask;
        g_inj_button_frames = g_inj_button_frames - 1;
        if (g_inj_button_frames == 0) g_inj_button_mask = 0;
    }
}

bool frame_card_requested() { return g_frame_card; }
}  // namespace detail
}  // namespace aiwatchos_mdb

// ---------------------------------------------------------------------------
// Safe-area calibration card (MDB `mdb frame`) — phase 1 of the safe-area
// calibration. The visible glass is smaller than the full 410x502 panel
// on every side (rounded corners + bezel mask eat the edges), so the UI
// safe area must be MEASURED, not guessed. While the card is latched,
// the main loop draws it over every finished frame (after the app and
// the status zone, before the band flush), so what reaches the glass is
// exactly this buffer: black field, concentric 1px borders at insets
// 0/8/16/24/32 (white/green/yellow/orange/red), edge ticks every 8 px,
// numeric coordinate labels every 16 px (legacy 5x7, scale 1), a centre
// crosshair and the title. The owner photographs the card on the glass;
// the outermost fully-visible border plus the tick labels give the four
// visible insets the phase-2 SAFE_* constants are built from. Latched
// in RAM only: reboot or `mdb frame off` clears it.
// ---------------------------------------------------------------------------
void draw_frame_card(aiwatchos::Framebuffer& fb) {
    using aiwatchos::rgb565_hex;
    const int W = fb.width;
    const int H = fb.height;
    const int cx = W / 2;
    const int cy = H / 2;
    const uint16_t kWhite = rgb565_hex(0xFFFFFF);
    fb.fill(0x0000);
    // Centre crosshair (cyan), under everything else.
    const uint16_t kCyan = rgb565_hex(0x00FFFF);
    fb.hline(0, W - 1, cy, kCyan);
    fb.vline(cx, 0, H - 1, kCyan);
    // Concentric borders at the calibration insets.
    static const int kInsets[5] = {0, 8, 16, 24, 32};
    static const uint32_t kBorderHex[5] = {0xFFFFFF, 0x00FF00, 0xFFFF00,
                                           0xFF8000, 0xFF0000};
    for (int i = 0; i < 5; ++i) {
        const int in = kInsets[i];
        const uint16_t c = rgb565_hex(kBorderHex[i]);
        fb.hline(in, W - 1 - in, in, c);
        fb.hline(in, W - 1 - in, H - 1 - in, c);
        fb.vline(in, in, H - 1 - in, c);
        fb.vline(W - 1 - in, in, H - 1 - in, c);
    }
    // Edge ticks every 8 px (white, 5 px long) along the top + left.
    for (int x = 0; x < W; x += 8) fb.vline(x, 0, 4, kWhite);
    for (int y = 0; y < H; y += 8) fb.hline(0, 4, y, kWhite);
    // Numeric labels every 16 px: the tick coordinate itself, drawn
    // just inside its edge, so a clipped label is measurement data too.
    char num[12];
    for (int x = 0; x < W; x += 16) {
        std::snprintf(num, sizeof(num), "%d", x);
        fb.draw_text(x + 2, 10, num, kWhite, 1);
    }
    for (int y = 0; y < H; y += 16) {
        std::snprintf(num, sizeof(num), "%d", y);
        fb.draw_text(10, y + 2, num, kWhite, 1);
    }
    // Title centred just above the crosshair arm; size line below it.
    const char* kTitle = "SAFE AREA TEST";
    const int tw = aiwatchos::Framebuffer::text_width(kTitle, 2);
    fb.draw_text(cx - tw / 2, cy - 32, kTitle, kWhite, 2);
    const char* kSub = "410 X 502";
    const int sw = aiwatchos::Framebuffer::text_width(kSub, 1);
    fb.draw_text(cx - sw / 2, cy + 16, kSub, kWhite, 1);
}

extern "C" void app_main(void) {
    // --- 0. MDB (Muse Developer Bridge, docs/mdb.md): log ring +
    // console task first, so the ring captures hardware init too.
    // No-op unless CONFIG_AIWATCHOS_MDB_ENABLE.
    aiwatchos_mdb::start();

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
    aiwatchos_mdb::detail::g_apps_hook = &g_app_mgr;
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
    // H10: tapping a live actionable notification (e.g. a Hermes
    // prompt) opens the producing app.
    if (auto* nm = aiwatchos::active_notifications()) {
        nm->set_action_handler(
            [](const char* app_id, void* ctx) {
                static_cast<aiwatchos::AppManager*>(ctx)->switch_to(app_id);
            },
            &g_app_mgr);
    }

    aiwatchos::App weather_app = aiwatchos::WeatherApp::make_app();
    g_app_mgr.register_app(&weather_app);

#ifdef __ESPRESSIF_IDF__
    // M22: provisioning status for the Settings About page. The OS
    // settings layer cannot depend on the net component, so main —
    // which links both — registers the provider. Presence/state words
    // ONLY: no secret value (token, password, key) is ever rendered.
    aiwatchos::settings_register_status_provider(
        [](aiwatchos::SettingsStatusRow* rows, size_t max_rows,
           void*) -> size_t {
            const aiwatchos_net::NetConfig& cfg = aiwatchos_net::net_config();
            size_t n = 0;
            auto add = [&](const char* label, const char* value) {
                if (n >= max_rows) return;
                std::snprintf(rows[n].label, sizeof(rows[n].label), "%s",
                              label);
                std::snprintf(rows[n].value, sizeof(rows[n].value), "%s",
                              value);
                ++n;
            };
            add("WiFi", cfg.wifi_configured()
                            ? aiwatchos_net::wifi_state_name(
                                  aiwatchos_net::wifi_state())
                            : "NOT SET");
            add("Muse token",
                (!cfg.hatch_token.empty() || !cfg.vm_token.empty())
                    ? "PRESENT"
                    : "MISSING");
            add("VM", cfg.vm_id.empty() ? "DEFAULT" : "SET");
            add("Muse link",
                aiwatchos_net::muse_link_state_name(
                    aiwatchos_net::muse_link_state()));
            add("Hermes pairing",
                aiwatchos_hermes::hermes_paired() ? "PAIRED" : "NOT PAIRED");
            add("Speech (TTS)",
                !aiwatchos_net::muse_tts_provisioned()
                    ? "NO KEY"
                    : (aiwatchos_net::muse_tts_speak_enabled() ? "ON"
                                                                : "OFF"));
            return n;
        },
        nullptr);

    // M10: the "Speak replies" toggle row in OS Settings (the volume
    // slider next to it already drives the HAL playback gain). State
    // lives in net config (NVS-persisted); the settings layer only
    // sees these callbacks.
    {
        aiwatchos::SettingsToggleRow speak_row;
        speak_row.label = "Speak replies";
        speak_row.icon = aiwatchos::Icon::kVolume;
        speak_row.get = [](void*) {
            return aiwatchos_net::muse_tts_speak_enabled();
        };
        speak_row.set = [](void*, bool on) {
            aiwatchos_net::muse_tts_set_speak_enabled(on);
        };
        aiwatchos::settings_register_toggle(speak_row);
    }
#endif

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
    aiwatchos_mdb::detail::g_power_hook = &g_power;

    // Touch event synthesis (Press/Move/Release from the FT3168 point
    // stream) and band-based dirty flushing (plan 1.6/1.7).
    // BOOT-hold escape detector (shell gesture; applied in the loop
    // at the input sampling point, above app dispatch).
    static aiwatchos::BootEscape g_boot_escape;
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
        uint32_t buttons = board.read_buttons();
        // MDB input injection (docs/mdb.md): applied here, before the
        // power policy samples the frame, so synthetic input is
        // indistinguishable from physical input downstream.
        aiwatchos_mdb::detail::apply_frame(&n_points, touch_xs, touch_ys,
                                           &buttons);

        // BOOT-hold escape (shell gesture, above app dispatch): BOOT
        // is also an app key (PTT / barge-in) and a capture-mode app
        // owns it completely, so the escape cannot live in any app.
        // Holding BOOT >= 3 s fires once per hold: switch to the
        // launcher, and swallow the rest of the hold (bit masked) so
        // no app ever acts on this press. The 3 s threshold sits
        // deliberately above the in-app BOOT-hold actions (Hermes
        // session.new / key reset fire at 2 s, in the app tick
        // below): an app hold always completes before the escape
        // can pre-empt it. Short presses pass through
        // untouched. Runs asleep too: the power policy below still
        // wakes the panel on the initial press, as before.
        if (g_boot_escape.update((buttons & 0x01u) != 0, frame_now_ms)) {
            g_app_mgr.switch_to("launcher");
        }
        if (g_boot_escape.latched()) buttons &= ~0x01u;

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
        // 12-hour, matching the clock face (stage-2 made the face
        // 12-hour per the design; the status-zone mini-clock kept
        // formatting 24-hour — same conversion as clock_face.cpp).
        const unsigned hour24 =
            static_cast<unsigned>((now_ms / 3600000) % 24);
        const unsigned hour12 = (hour24 % 12 == 0) ? 12u : (hour24 % 12);
        unsigned minutes = static_cast<unsigned>((now_ms / 60000) % 60);
        snprintf(time_str, sizeof(time_str), "%u:%02u", hour12, minutes);
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
            fb.fill_rect(0, aiwatchos::kStatusZoneBottom - 3,
                         aiwatchos::kDisplayWidth - 1,
                         aiwatchos::kStatusZoneBottom - 1, link_color);
        }
#endif

        // MDB safe-area calibration card (`mdb frame`): while latched,
        // overdraw the finished frame — app + status zone and all — so
        // the glass shows the card instead of the app. Drawn here, after
        // every other render and before the band scan, the card is part
        // of the flushed frame like anything else.
        if (aiwatchos_mdb::detail::frame_card_requested()) {
            draw_frame_card(fb);
        }

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
