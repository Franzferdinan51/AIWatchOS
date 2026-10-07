// AppManager unit test — verifies register/launch/tick/render dispatch logic.
// This test is pure C++ (no ESP-IDF dependencies) and can be compiled with any
// standard compiler: g++ -std=c++17 app_registry_test.cpp ...
// It exercises the real shipped AppManager code from src/app_manager.cpp and
// the framebuffer helpers from src/app.cpp.
#include "aiwatchos/app.hpp"
#include "aiwatchos/app_manager.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace aiwatchos {

// --- Mock HAL for testing (no hardware) ---
class TestHal : public Hal {
 public:
    bool display_flush() override { return true; }
    void set_backlight(uint8_t p) override { backlight_ = p; }
    int read_touch(int* xs, int* ys) override { *xs = 0; *ys = 0; return 0; }
    bool audio_start_playback(uint32_t r) override { rate_ = r; return true; }
    void audio_write(const int16_t*, size_t) override {}
    void audio_stop_playback() override {}
    bool mic_start(uint32_t r) override { mic_rate_ = r; return true; }
    void mic_stop() override {}
    PowerStatus read_power() override {
        PowerStatus ps{};
        ps.battery_present = true;
        ps.battery_percent = 85;
        ps.battery_mv = 3800;
        ps.charging = false;
        ps.external_power = false;
        return ps;
    }
    uint64_t now_ms() override { return static_cast<uint64_t>(ms_); }
    uint32_t read_buttons() override { return 0; }

    void advance_ms(uint64_t ms) { ms_ += ms; }
 private:
    uint8_t backlight_ = 100;
    uint32_t rate_ = 16000, mic_rate_ = 16000;
    uint64_t ms_ = 0;
};

}  // namespace aiwatchos

// --- Test app callbacks with observable state ---
static bool g_app_a_init_called = false;
static int g_app_a_tick_count = 0;
static bool g_app_a_rendered = false;
static int g_app_b_init_called = false;
static bool g_app_b_touch_received = false;

static void app_a_init() { g_app_a_init_called = true; }
static void app_a_tick(uint32_t) { g_app_a_tick_count++; }
static void app_a_render(aiwatchos::Framebuffer& fb) {
    // Fill the framebuffer — proves render dispatch works.
    for (int i = 0; i < fb.width * fb.height; ++i) fb.pixels[i] = 0xFFFF;
    g_app_a_rendered = true;
}

static void app_b_init() { g_app_b_init_called = true; }
static bool app_b_touch(const aiwatchos::TouchEvent& e) {
    (void)e;
    g_app_b_touch_received = true;
    return true;  // consumed
}

static void reset_test_state() {
    g_app_a_init_called = false;
    g_app_a_tick_count = 0;
    g_app_a_rendered = false;
    g_app_b_init_called = false;
    g_app_b_touch_received = false;
}

// --- Test framebuffer bounds checking ---
static void test_framebuffer_bounds() {
    uint16_t buf[410 * 502];
    aiwatchos::Framebuffer fb{buf, 410, 502};

    fb.fill(0x1234);
    // Verify every pixel was set.
    for (int i = 0; i < 410 * 502; ++i) {
        assert(buf[i] == 0x1234);
    }

    // Out-of-bounds writes must be silently ignored (no crash).
    fb.put_pixel(-1, -1, 0xFFFF);       // before origin
    fb.put_pixel(410, 502, 0xFFFF);      // past end
    assert(buf[0] == 0x1234);            // unchanged

    // In-bounds write works.
    fb.put_pixel(0, 0, 0xABCD);
    assert(buf[0] == 0xABCD);
    fb.put_pixel(409, 501, 0xABCE);      // last valid pixel
    assert(buf[501 * 410 + 409] == 0xABCE);

    printf("test_framebuffer_bounds: PASSED\n");
}

// --- Test app registry registration and dispatch ---
static void test_app_registry() {
    aiwatchos::TestHal hal;
    aiwatchos::AppManager mgr(hal);

    // Register two apps without auto-launch.
    aiwatchos::App app_a = aiwatchos::make_app("appA", app_a_init, app_a_tick, app_a_render);
    aiwatchos::App app_b = aiwatchos::make_app("appB", app_b_init, nullptr, nullptr, app_b_touch);

    assert(mgr.register_app(&app_a) == true);
    assert(mgr.register_app(&app_b) == true);
    assert(mgr.count() == 2);

    // Duplicate id must be rejected.
    aiwatchos::App app_dup = aiwatchos::make_app("appA");
    assert(mgr.register_app(&app_dup) == false);
    assert(mgr.count() == 2);   // no growth on duplicate

    printf("test_app_registry: PASSED\n");
}

// --- Test launch, tick dispatch, and render dispatch ---
static void test_launch_and_dispatch() {
    reset_test_state();
    aiwatchos::TestHal hal;
    aiwatchos::AppManager mgr(hal);

    uint16_t buf[410 * 502];
    aiwatchos::Framebuffer fb{buf, 410, 502};

    // Register app_a with launch_now=true.
    aiwatchos::App app_a = aiwatchos::make_app("appA", app_a_init, app_a_tick, app_a_render);
    assert(mgr.register_app(&app_a, true) == true);
    assert(g_app_a_init_called == true);   // init was called on launch

    // Tick should dispatch to the foreground app's tick callback.
    mgr.tick(40);
    assert(g_app_a_tick_count >= 1);   // at least one tick dispatched

    // Render should call app_a_render, which fills the framebuffer white.
    fb.fill(0x0000);   // start black
    mgr.render(fb);
    assert(g_app_a_rendered == true);
    assert(buf[0] == 0xFFFF);          // first pixel set by render callback

    printf("test_launch_and_dispatch: PASSED\n");
}

// --- Test touch dispatch and app switching ---
static void test_touch_and_switch() {
    reset_test_state();
    aiwatchos::TestHal hal;
    aiwatchos::AppManager mgr(hal);

    // Register two apps. Launch app_a first, then switch to app_b.
    aiwatchos::App app_a = aiwatchos::make_app("appA", nullptr, nullptr, nullptr);
    aiwatchos::App app_b = aiwatchos::make_app("appB", app_b_init, nullptr, nullptr, app_b_touch);

    assert(mgr.register_app(&app_a) == true);
    assert(mgr.register_app(&app_b) == true);

    // Switch to appA, then switch to appB.
    assert(mgr.switch_to("appA") == true);
    assert(std::string(mgr.current_app_id()) == "appA");
    assert(g_app_a_init_called == false);   // was not launched with launch_now; init only on switch

    assert(mgr.switch_to("appB") == true);
    assert(std::string(mgr.current_app_id()) == "appB");
    assert(g_app_b_init_called == true);    // init called during switch

    // Touch should dispatch to appB's touch callback.
    aiwatchos::TouchEvent te{100, 200, aiwatchos::TouchEvent::Press};
    bool consumed = mgr.on_touch(te);
    assert(consumed == true);
    assert(g_app_b_touch_received == true);

    // switch_to with non-existent id must fail.
    assert(mgr.switch_to("nonexistent") == false);
    assert(std::string(mgr.current_app_id()) == "appB");  // unchanged

    printf("test_touch_and_switch: PASSED\n");
}

// --- Test switch_next / switch_prev cycle through all registered apps ---
static void test_switch_cycle() {
    aiwatchos::TestHal hal;
    aiwatchos::AppManager mgr(hal);

    aiwatchos::App app_a = aiwatchos::make_app("appA");
    aiwatchos::App app_b = aiwatchos::make_app("appB");
    aiwatchos::App app_c = aiwatchos::make_app("appC");

    assert(mgr.register_app(&app_a, true) == true);   // launch appA first
    mgr.register_app(&app_b);
    mgr.register_app(&app_c);

    assert(std::string(mgr.current_app_id()) == "appA");
    mgr.switch_next();
    assert(std::string(mgr.current_app_id()) == "appB");
    mgr.switch_next();
    assert(std::string(mgr.current_app_id()) == "appC");
    mgr.switch_next();   // wraps around to appA
    assert(std::string(mgr.current_app_id()) == "appA");

    mgr.switch_prev();
    assert(std::string(mgr.current_app_id()) == "appC");

    printf("test_switch_cycle: PASSED\n");
}

int main() {
    test_framebuffer_bounds();
    test_app_registry();
    test_launch_and_dispatch();
    test_touch_and_switch();
    test_switch_cycle();

    printf("\nAll AppManager tests PASSED.\n");
    return 0;
}
