// AIWatchOS settings page — Muse Gloss rows (DESIGN_TOKENS section 7,
// plan 3.4): bubble rows with an icon chip, a label, and the control
// on the right — REAL sliders for brightness/volume (press + drag,
// forwarded to the HAL on change), a switch for Do Not Disturb, an
// About row with the live battery value, and a Color Test row that
// opens the byte-order pattern (plan 0.1). The old Theme row is gone:
// it was a control that did nothing (audit F2) and honest UI does not
// ship lying controls. About now opens the provisioning-status page
// (M22) fed by the registered status provider, and one external
// toggle row may be registered below the built-ins (see below).
#pragma once

#include <cstddef>
#include <cstdint>
#include "aiwatchos/app.hpp"
#include "aiwatchos/hal.hpp"
#include "aiwatchos/icons.hpp"

namespace aiwatchos {

// --- Registered rows (seams for layers the OS cannot depend on) ----------
// Components like aiwatchos_net sit ABOVE the OS layer, so this page
// cannot read their state directly; main wires these at boot instead.
// A status provider fills label/value rows for the About page (M22:
// provisioning status — presence/state words only, never the secret
// values themselves). A toggle row adds ONE switch row bound to
// external get/set callbacks (M10's "Speak replies").
struct SettingsStatusRow {
    char label[24] = {};
    char value[40] = {};
};
using SettingsStatusProvider = size_t (*)(SettingsStatusRow* rows,
                                          size_t max_rows, void* ctx);
void settings_register_status_provider(SettingsStatusProvider fn, void* ctx);

struct SettingsToggleRow {
    const char* label = nullptr;
    Icon icon = Icon::kSparkle;
    bool (*get)(void* ctx) = nullptr;
    void (*set)(void* ctx, bool on) = nullptr;
    void* ctx = nullptr;
};
// The 502 px screen fits the 5 built-in rows plus exactly one external
// toggle row; returns false when the slot is taken or the row is
// incomplete (no label/get/set).
bool settings_register_toggle(const SettingsToggleRow& row);

struct SettingsState {
    uint8_t brightness = 100;   // 5..100 (matches HAL constraints)
    uint8_t volume = 70;        // 0..100
    bool do_not_disturb = false;
};

class SettingsPage {
 public:
    explicit SettingsPage(Hal& board);

    void render(Framebuffer& fb);
    bool on_touch(const TouchEvent& event);

    static App make_app();

 private:
    void set_slider_value(int row, int x);

    Hal& board_;
    SettingsState state_{};
    bool color_test_ = false;   // colour-test pattern is showing
    bool about_open_ = false;   // About (provisioning status) page showing
    int drag_row_ = -1;         // slider row being dragged (-1 = none)

    void render_about(Framebuffer& fb);
};

}  // namespace aiwatchos
