// Launcher UI — Muse Gloss tile grid. See launcher_ui.hpp.
#include "aiwatchos/launcher_ui.hpp"
#include "aiwatchos/app_manager.hpp"
#include "aiwatchos/colors.hpp"
#include "aiwatchos/font.hpp"

#include <cstring>

namespace aiwatchos {
namespace {

// Per-app tile identity: distinct icon + short label per app (labels
// are capped at what fits a 96px tile in Micro — "Notifications" at
// ~98px does not fit, so the bell tile reads "Alerts").
struct TileDef {
    const char* id;
    const char* label;
    Icon icon;
};

const TileDef kTileDefs[] = {
    {"clock", "Clock", Icon::kClock},
    {"launcher", "Apps", Icon::kSparkle},
    {"muse", "Muse", Icon::kMic},
    {"hermes", "Hermes", Icon::kFeather},
    {"settings", "Settings", Icon::kGear},
    {"notifications", "Alerts", Icon::kBell},
    {"weather", "Weather", Icon::kCloudSun},
};

const TileDef* tile_def_for(const char* id) {
    for (const TileDef& def : kTileDefs) {
        if (std::strcmp(def.id, id) == 0) return &def;
    }
    return nullptr;
}

}  // namespace

LauncherUI::LauncherUI(AppManager& mgr) : mgr_(mgr) {}

void LauncherUI::render(Framebuffer& fb) {
    // No background fill: tiles float on the shell's G5 backdrop.
    const size_t count = mgr_.count();
    for (size_t i = 0; i < count; ++i) {
        App* app = mgr_.app_at_index(i);
        if (!app || !app->id) continue;
        int x, y;
        tile_origin(static_cast<int>(i), x, y);
        const bool is_hermes = std::strcmp(app->id, "hermes") == 0;

        BubbleStyle style = bubble_style();
        if (is_hermes) {
            // Gold identity (tokens section 9): Hermes stops + gold
            // shadow/icon/foot; the geometry is identical to Muse's.
            style.top = hcolors::kBarTop;
            style.mid = hcolors::kBar;
            style.bot = hcolors::kBarBot;
            style.shadow_color = hcolors::kGold;
        }
        style.shadow = shadow_tile();
        fb.draw_bubble(x, y, kTileSize, kTileSize, 24, style);

        const TileDef* def = tile_def_for(app->id);
        const Icon icon = def ? def->icon : Icon::kSparkle;
        const char* label = def ? def->label : app->id;
        const uint16_t fg = is_hermes ? hcolors::kGold : colors::kText;
        draw_icon(fb, x + 28, y + 14, icon, FontFace::kIcon40, fg);
        draw_text_aa_centered(fb, x + kTileSize / 2, y + 62, label,
                              FontFace::kMicro, fg);
        if (is_hermes) {
            fb.fill_round_rect(x + 36, y + 84, 24, 4, 2, hcolors::kGold);
        }

        // Press feedback: +12% white wash over the tile for ~3 renders.
        if (pressed_frames_ > 0 && static_cast<int>(i) == pressed_idx_) {
            fb.fill_round_rect(x, y, kTileSize, kTileSize, 24,
                               blend565(style.mid, colors::kWhite, 31));
        }
    }
    if (pressed_frames_ > 0) --pressed_frames_;
}

bool LauncherUI::on_touch(const TouchEvent& event) {
    if (event.type != TouchEvent::Press) return false;

    // Tiles first — they start at x=33, clear of the edge strips, and a
    // tap in a gutter/gap must launch nothing (the hit test pins this).
    const size_t count = mgr_.count();
    for (size_t i = 0; i < count; ++i) {
        int x, y;
        tile_origin(static_cast<int>(i), x, y);
        if (event.x >= x && event.x < x + kTileSize && event.y >= y &&
            event.y < y + kTileSize) {
            App* target = mgr_.app_at_index(i);
            if (target && target->id) {
                pressed_idx_ = static_cast<int>(i);
                pressed_frames_ = 3;
                return mgr_.switch_to(target->id);
            }
        }
    }

    // Edge-swipe navigation: left/right 30px strips switch apps.
    if (event.x < 30) {
        mgr_.switch_prev();
        return true;
    }
    if (event.x > kDisplayWidth - 30) {
        mgr_.switch_next();
        return true;
    }
    return false;
}

}  // namespace aiwatchos
