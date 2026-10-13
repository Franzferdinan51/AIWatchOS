// Notification card list — see notifications.hpp.
#include "aiwatchos/notifications.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "aiwatchos/battery_display.hpp"   // kStatusZoneH
#include "aiwatchos/colors.hpp"
#include "aiwatchos/font.hpp"

namespace aiwatchos {
namespace {

constexpr int kMaxNotifications = 16;   // bounded buffer to limit PSRAM usage
constexpr int kCardX = 20;
constexpr int kCardW = 370;
constexpr int kListY = kStatusZoneBottom;
constexpr int kCardGap = 12;
constexpr int kBodyX = 64;              // text starts right of the bell chip
constexpr int kBodyW = 294;             // kCardW - kBodyX - 12

// Pixel-width greedy wrap into at most max_lines lines (a negative
// max_lines means unbounded). When the text needs more lines than
// allowed, the last line is truncated with an ellipsis.
std::vector<std::string> wrap_px(const std::string& text, FontFace face,
                                 int max_w, int max_lines) {
    std::vector<std::string> lines;
    std::string cur;
    size_t i = 0;
    auto flush = [&]() {
        lines.push_back(cur);
        cur.clear();
    };
    auto fits = [&](const std::string& s) {
        return text_width_aa(s, face) <= max_w;
    };
    while (i < text.size()) {
        if (text[i] == '\n') {
            flush();
            ++i;
            continue;
        }
        if (text[i] == ' ') {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < text.size() && text[j] != ' ' && text[j] != '\n') ++j;
        std::string word = text.substr(i, j - i);
        i = j;
        // Hard-break a word that alone exceeds the line.
        while (!fits(word)) {
            if (!cur.empty()) {
                flush();
                if (max_lines >= 0 &&
                    static_cast<int>(lines.size()) >= max_lines) {
                    return lines;
                }
            }
            // Longest prefix that fits.
            size_t cut = word.size();
            while (cut > 1 && !fits(word.substr(0, cut))) --cut;
            lines.push_back(word.substr(0, cut));
            word = word.substr(cut);
            if (max_lines >= 0 &&
                static_cast<int>(lines.size()) >= max_lines) {
                return lines;
            }
        }
        if (cur.empty()) {
            cur = word;
        } else if (fits(cur + " " + word)) {
            cur += " " + word;
        } else {
            flush();
            if (max_lines >= 0 &&
                static_cast<int>(lines.size()) >= max_lines) {
                return lines;
            }
            cur = word;
        }
    }
    if (!cur.empty() || lines.empty()) flush();
    // Ellipsis when content was cut off by the line cap.
    if (max_lines >= 0 && static_cast<int>(lines.size()) >= max_lines &&
        i < text.size()) {
        std::string& last = lines.back();
        while (!last.empty() && !fits(last + "...")) {
            last.pop_back();
        }
        last += "...";
    }
    return lines;
}

// Truncate a single line to max_w px with a trailing ellipsis.
std::string truncate_px(const std::string& text, FontFace face, int max_w) {
    if (text_width_aa(text, face) <= max_w) return text;
    std::string out = text;
    while (!out.empty() && text_width_aa(out + "...", face) > max_w) {
        out.pop_back();
    }
    return out + "...";
}

// Card height for a given number of body lines.
int card_height(int body_line_count) {
    return 36 + body_line_count * font_line_height(FontFace::kCaption) + 10;
}

}  // namespace

NotificationManager::NotificationManager(Hal& board) : board_(board) {}

void NotificationManager::add_notification(std::string title, std::string body) {
    if (notifications_.size() >= kMaxNotifications) {
        notifications_.erase(notifications_.begin());   // drop oldest
    }
    Notification n;
    n.title = std::move(title);
    n.body = std::move(body);
    n.timestamp_ms = board_.now_ms();
    notifications_.push_back(std::move(n));
    expanded_ = -1;
}

void NotificationManager::add_actionable(std::string title, std::string body,
                                         std::string action_app,
                                         std::string action_id,
                                         uint64_t expires_ms) {
    // Dedupe: one persistent entry per (app, id), updated in place.
    for (auto& n : notifications_) {
        if (!n.action_app.empty() && n.action_app == action_app &&
            n.action_id == action_id) {
            n.title = std::move(title);
            n.body = std::move(body);
            n.expires_ms = expires_ms;
            n.timestamp_ms = board_.now_ms();
            return;
        }
    }
    if (notifications_.size() >= kMaxNotifications) {
        notifications_.erase(notifications_.begin());   // drop oldest
    }
    Notification n;
    n.title = std::move(title);
    n.body = std::move(body);
    n.timestamp_ms = board_.now_ms();
    n.action_app = std::move(action_app);
    n.action_id = std::move(action_id);
    n.expires_ms = expires_ms;
    notifications_.push_back(std::move(n));
    expanded_ = -1;
}

bool NotificationManager::retire_action(const std::string& action_app,
                                        const std::string& action_id) {
    for (size_t i = 0; i < notifications_.size(); ++i) {
        const Notification& n = notifications_[i];
        if (!n.action_app.empty() && n.action_app == action_app &&
            n.action_id == action_id) {
            notifications_.erase(notifications_.begin() + i);
            if (expanded_ == static_cast<int>(i)) expanded_ = -1;
            return true;
        }
    }
    return false;
}

bool NotificationManager::has_action(const std::string& action_app,
                                     const std::string& action_id) const {
    for (const auto& n : notifications_) {
        if (!n.action_app.empty() && n.action_app == action_app &&
            n.action_id == action_id) {
            return true;
        }
    }
    return false;
}

int NotificationManager::body_lines(const Notification& n, bool expanded) const {
    if (expanded) {
        return static_cast<int>(wrap_px(n.body, FontFace::kCaption, kBodyW, -1).size());
    }
    return static_cast<int>(wrap_px(n.body, FontFace::kCaption, kBodyW, 2).size());
}

void NotificationManager::render(Framebuffer& fb) {
    if (notifications_.empty()) {
        // Empty state: bell in a bubble + a quiet line (tokens 3.5).
        BubbleStyle style = bubble_style();
        style.shadow = nullptr;   // small bubble; sprite is tile-sized
        fb.draw_bubble(kDisplayWidth / 2 - 40, 190, 80, 80, 24, style);
        draw_icon(fb, kDisplayWidth / 2 - 20, 210, Icon::kBell,
                  FontFace::kIcon40, colors::kDim);
        draw_text_aa_centered(fb, kDisplayWidth / 2, 292,
                              "No notifications", FontFace::kBody,
                              colors::kDim);
        return;
    }

    int y = kListY - scroll_offset_;
    // Newest first: walk the vector backwards.
    for (int idx = static_cast<int>(notifications_.size()) - 1; idx >= 0;
         --idx) {
        const Notification& n = notifications_[idx];
        const bool expanded = (idx == expanded_);
        const auto lines =
            wrap_px(n.body, FontFace::kCaption, kBodyW, expanded ? -1 : 2);
        const int h = card_height(static_cast<int>(lines.size()));
        if (y + h >= kStatusZoneBottom && y < kDisplayHeight) {
            // Card shadow: the baked card sprite stretched vertically
            // to this card's height (its reference height is 152).
            if (const AlphaSprite* shadow = shadow_card()) {
                int cap_top = 0, cap_bot = 0;
                card_shadow_caps(cap_top, cap_bot);
                fb.blit_alpha4_vstretch(kCardX + shadow->anchor_x - 11,
                                        y + shadow->anchor_y, *shadow,
                                        colors::kBlue, h + 32, cap_top,
                                        cap_bot);
            }
            BubbleStyle style = bubble_style();
            style.shadow = nullptr;   // pre-blitted stretched above
            fb.draw_bubble(kCardX, y, kCardW, h, 18, style);

            // Bell chip.
            const uint16_t chip =
                blend565(colors::kBubble, colors::kBlue, 46);
            fb.fill_round_rect(kCardX + 12, y + 12, 40, 40, 20, chip);
            draw_icon(fb, kCardX + 22, y + 22, Icon::kBell,
                      FontFace::kIcon20, colors::kBlueTop);

            // Title (ellipsis-truncated) + receipt time, right.
            const std::string title =
                truncate_px(n.title, FontFace::kBodySemibold, 218);
            draw_text_aa(fb, kCardX + kBodyX, y + 10, title,
                         FontFace::kBodySemibold, colors::kText);
            const uint64_t shown_ms = local_display_ms(n.timestamp_ms);
            char time_buf[8];
            std::snprintf(time_buf, sizeof(time_buf), "%u:%02u",
                          static_cast<unsigned>((shown_ms / 3600000) % 24),
                          static_cast<unsigned>((shown_ms / 60000) % 60));
            draw_text_aa_right(fb, kCardX + kCardW - 12, y + 12, time_buf,
                               FontFace::kMicro, colors::kFaint);
            // H10: an expired actionable entry says so, in place of
            // any affordance — it must never read as tappable.
            if (expired(n)) {
                draw_text_aa_right(fb, kCardX + kCardW - 12, y + 24,
                                   "EXPIRED", FontFace::kMicro,
                                   colors::kDanger);
            }

            // Body lines.
            const int lh = font_line_height(FontFace::kCaption);
            for (size_t li = 0; li < lines.size(); ++li) {
                draw_text_aa(fb, kCardX + kBodyX, y + 36 + static_cast<int>(li) * lh,
                             lines[li], FontFace::kCaption, colors::kDim);
            }
        }
        y += h + kCardGap;
    }
}

bool NotificationManager::on_touch(const TouchEvent& event) {
    if (notifications_.empty()) return false;

    // Total stack height for scroll clamping.
    auto stack_height = [&]() {
        int total = 0;
        for (size_t i = 0; i < notifications_.size(); ++i) {
            total += card_height(body_lines(
                         notifications_[i],
                         static_cast<int>(i) == expanded_)) +
                     kCardGap;
        }
        return total;
    };
    // Card index (into notifications_) at screen y, or -1.
    auto card_at = [&](int screen_y) {
        int y = kListY - scroll_offset_;
        for (int idx = static_cast<int>(notifications_.size()) - 1;
             idx >= 0; --idx) {
            const int h = card_height(
                body_lines(notifications_[idx], idx == expanded_));
            if (screen_y >= y && screen_y < y + h) return idx;
            y += h + kCardGap;
        }
        return -1;
    };

    switch (event.type) {
        case TouchEvent::Press:
            press_x_ = event.x;
            press_y_ = event.y;
            last_y_ = event.y;
            press_card_ = card_at(event.y);
            drag_moved_ = false;
            return true;
        case TouchEvent::Move: {
            const int dy = last_y_ - event.y;
            last_y_ = event.y;
            if (event.y - press_y_ > 8 || press_y_ - event.y > 8 ||
                event.x - press_x_ > 8 || press_x_ - event.x > 8) {
                drag_moved_ = true;
            }
            scroll_offset_ += dy;
            const int max_scroll =
                std::max(0, stack_height() - (kDisplayHeight - kListY));
            if (scroll_offset_ < 0) scroll_offset_ = 0;
            if (scroll_offset_ > max_scroll) scroll_offset_ = max_scroll;
            return true;
        }
        case TouchEvent::Release: {
            const bool consumed = press_card_ >= 0;
            if (!drag_moved_ && press_card_ >= 0) {
                // Tap on a LIVE actionable entry fires its action
                // (H10) instead of expanding. An expired entry never
                // fires — it falls through to plain expand behavior.
                const Notification& tapped = notifications_[press_card_];
                if (!tapped.action_app.empty() && !expired(tapped) &&
                    action_handler_) {
                    const std::string app = tapped.action_app;
                    action_handler_(app.c_str(), action_ctx_);
                } else {
                    // Tap: toggle expansion of the pressed card.
                    expanded_ = (expanded_ == press_card_) ? -1 : press_card_;
                }
            } else if (drag_moved_ && press_card_ >= 0 &&
                       event.x - press_x_ > 60) {
                // Swipe right: dismiss.
                notifications_.erase(notifications_.begin() + press_card_);
                if (expanded_ == press_card_) expanded_ = -1;
                press_card_ = -1;
                const int max_scroll =
                    std::max(0, stack_height() - (kDisplayHeight - kListY));
                if (scroll_offset_ > max_scroll) scroll_offset_ = max_scroll;
                return true;
            }
            press_card_ = -1;
            return consumed;
        }
    }
    return false;
}

// Static state for callback-based app interface.
namespace {
    NotificationManager* g_notifications = nullptr;
}  // namespace

NotificationManager* active_notifications() { return g_notifications; }

void set_active_notifications(NotificationManager* mgr) {
    g_notifications = mgr;
}

namespace {

    void notif_init() {}

    void notif_tick(uint32_t elapsed_ms) {
        (void)elapsed_ms;
        // In a real build, this polls the BLE characteristic or AI message queue
        // for new notifications from paired devices. The stub adds one example notification
        // on first tick to demonstrate the UI.
        static bool seeded = false;
        if (!seeded && g_notifications) {
            g_notifications->add_notification("System", "AIWatchOS started. Tap notifications to scroll.");
            seeded = true;
        }
    }

    void notif_render(Framebuffer& fb) {
        if (g_notifications) g_notifications->render(fb);
    }

    bool notif_touch(const TouchEvent& event) {
        if (g_notifications) return g_notifications->on_touch(event);
        return false;
    }
}

App NotificationManager::make_app() {
    static Hal& board = hal();
    static NotificationManager mgr(board);
    set_active_notifications(&mgr);

    // Fully qualify to avoid resolving as a recursive call to this member function.
    return aiwatchos::make_app("notifications", notif_init, notif_tick, notif_render, notif_touch);
}

}  // namespace aiwatchos
