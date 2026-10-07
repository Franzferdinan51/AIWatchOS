// Launcher UI — touch-driven app grid with tap-to-launch and edge-swipe navigation.
// Renders on the 410x502 portrait AMOLED, below a status bar. Touch coordinates
// from FT3168 map to screen space: x in [0, 409], y in [0, 501].
#include "aiwatchos/launcher_ui.hpp"
#include "aiwatchos/app_manager.hpp"

namespace aiwatchos {

constexpr uint16_t kBgColor     = 0x0000;   // black background
constexpr uint16_t kLabelColor  = 0xFFFF;   // white text labels
constexpr uint16_t kIconColor   = 0x7BEF;   // icon squares (medium gray)

LauncherUI::LauncherUI(AppManager& mgr) : mgr_(mgr) {}

void LauncherUI::render(Framebuffer& fb) {
    fb.fill(kBgColor);

    const int status_bar_h = 36;
    const int icon_size = 64;
    const int cols = 3;
    const int gap_x = (kDisplayWidth - cols * icon_size) / (cols + 1);

    // Draw app icons in a grid starting below the status bar.
    size_t idx = 0;
    for (int row = 0; ; ++row) {
        int y = status_bar_h + 24 + row * (icon_size + 36);
        if (y > kDisplayHeight - icon_size) break;

        for (int col = 0; col < cols; ++col) {
            // Check we still have apps to render.
            int total_rows = static_cast<int>(mgr_.count() / cols) + 1;
            if (row >= total_rows || idx >= mgr_.count()) break;

            // Icon placeholder (rounded square). Real impl would render app icon from assets/
            int x = gap_x + col * (icon_size + gap_x);

            // Draw icon placeholder (rounded square).
            fb.fill_rect(x, y, x + icon_size - 1, y + icon_size - 1, kIconColor);

            idx++;
        }
    }
}

bool LauncherUI::on_touch(const TouchEvent& event) {
    // Edge-swipe: touch in the left/right 30px triggers app switch.
    if (event.x < 30 && event.type == TouchEvent::Press) {
        mgr_.switch_prev();
        return true;
    }
    if (event.x > kDisplayWidth - 30 && event.type == TouchEvent::Press) {
        mgr_.switch_next();
        return true;
    }

    // Tap-to-launch: find which app icon was tapped and switch to it.
    const int status_bar_h = 36;
    const int icon_size = 64;
    const int cols = 3;
    const int gap_x = (kDisplayWidth - cols * icon_size) / (cols + 1);

    if (event.y >= status_bar_h + 24 && event.type == TouchEvent::Press) {
        int rel_y = event.y - (status_bar_h + 24);
        int row = rel_y / (icon_size + 36);
        int col = (event.x - gap_x) / (icon_size + gap_x);

        if (col >= 0 && col < cols && row >= 0) {
            // Reject taps landing in the gaps: integer division truncates
            // toward zero, so without this a tap left of the grid (or in a
            // gutter) would round into a neighboring icon and launch it.
            int icon_x = gap_x + col * (icon_size + gap_x);
            int icon_y = status_bar_h + 24 + row * (icon_size + 36);
            if (event.x < icon_x || event.x >= icon_x + icon_size ||
                event.y < icon_y || event.y >= icon_y + icon_size) {
                return false;
            }
            size_t target_idx = static_cast<size_t>(row * cols + col);
            App* target = mgr_.app_at_index(target_idx);
            if (target && target->id) {
                return mgr_.switch_to(target->id);   // true: consumed and switched
            }
        }
    }

    return false;
}

}  // namespace aiwatchos
