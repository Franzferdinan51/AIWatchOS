// Notification display — renders messages with scroll support on the 410x502 AMOLED.
// Touch-drag scrolls vertically; tap returns to the latest notification. Designed for
// hardware constraints: uses std::vector (heap-allocated in PSRAM via ESP-IDF config)
// but limits notifications to a bounded deque-style buffer (max 16 entries).
#include "aiwatchos/notifications.hpp"
#include <cstring>

namespace aiwatchos {

constexpr uint16_t kBgColor     = 0x0000;   // black AMOLED background
constexpr uint16_t kHeaderCol   = 0x294A;   // dark blue header band
constexpr uint16_t kTitleColor  = 0xFFFF;   // white title text
constexpr uint16_t kBodiesColor = 0xBDEF;   // light gray body text

constexpr int kMaxNotifications = 16;   // bounded buffer to limit PSRAM usage

NotificationManager::NotificationManager(Hal& board) : board_(board) {}

void NotificationManager::add_notification(std::string title, std::string body) {
    if (notifications_.size() >= kMaxNotifications) {
        notifications_.erase(notifications_.begin());   // drop oldest
    }
    notifications_.push_back({std::move(title), std::move(body), board_.now_ms()});
}

void NotificationManager::render(Framebuffer& fb) {
    if (notifications_.empty()) {
        // Empty state: show "No notifications" centered on screen.
        fb.fill(kBgColor);
        const char* msg = "NO NOTIFICATIONS";
        int x = (kDisplayWidth - static_cast<int>(strlen(msg)) * 9) / 2;
        int y = kDisplayHeight / 2 - 8;
        for (size_t i = 0; msg[i]; ++i) {
            int cx = x + static_cast<int>(i) * 9;
            fb.fill_rect(cx, y, cx + 7, y + 15, kTitleColor);
        }
        return;
    }

    // Show the latest notification (index = last).
    const Notification& n = notifications_.back();

    // Status bar.
    fb.fill_rect(0, 0, kDisplayWidth - 1, 36, kHeaderCol);

    // Title at top of content area.
    int title_y = 52;
    for (size_t c = 0; n.title[c] && c < 40; ++c) {   // cap at ~40 chars per line
        int cx = 16 + static_cast<int>(c) * 9;
        fb.fill_rect(cx, title_y, cx + 7, title_y + 15, kTitleColor);
    }

    // Body text below the title — scroll support via scroll_offset_.
    const int body_start_x = 16;
    const int body_start_y = 84;
    const int chars_per_line = (kDisplayWidth - 32) / 9;   // ~44 chars at 8px+1gap

    // Simple text wrapping: break into lines of chars_per_line.
    std::string remaining = n.body;
    int line_y = body_start_y - scroll_offset_;

    while (!remaining.empty() && line_y < kDisplayHeight - 36) {
        // Consume exactly one line per iteration so every line is reached;
        // draw only the lines inside the visible area (scrolling just shifts
        // line_y). The old code broke out after the first word here.
        size_t break_pos = remaining.find_first_of(" \n");
        std::string word;
        if (break_pos != std::string::npos && static_cast<int>(break_pos) < chars_per_line) {
            word = remaining.substr(0, break_pos);
            remaining = remaining.substr(break_pos + 1);   // skip the space/newline
        } else if (remaining.length() > static_cast<size_t>(chars_per_line)) {
            word = remaining.substr(0, chars_per_line);
            remaining = remaining.substr(chars_per_line);
        } else {
            word = remaining;
            remaining.clear();
        }

        if (line_y >= body_start_y) {   // only draw lines in the visible area
            for (size_t c = 0; c < word.length() && c < static_cast<size_t>(chars_per_line); ++c) {
                int cx = body_start_x + static_cast<int>(c) * 9;
                if (cx < kDisplayWidth - 16) {
                    fb.fill_rect(cx, line_y, cx + 7, line_y + 15, kBodiesColor);
                }
            }
        }

        line_y += 20;   // ~8px font height + line spacing
    }

    (void)n.timestamp_ms;   // timestamp is available but not rendered in this simplified version
}

bool NotificationManager::on_touch(const TouchEvent& event) {
    if (notifications_.empty()) return false;

    // Vertical drag to scroll. Horizontal swipe dismisses the current notification.
    static int last_y = 0;

    if (event.type == TouchEvent::Press) {
        last_y = event.y;
        return true;
    }

    if (event.type == TouchEvent::Move) {
        // Scroll: drag up to scroll content down, and vice versa.
        int delta = last_y - event.y;   // positive = dragged up = scroll content up
        scroll_offset_ += delta;
        if (scroll_offset_ < 0) scroll_offset_ = 0;

        last_y = event.y;
        return true;
    }

    if (event.type == TouchEvent::Release) {
        // Tap (press and release near each other) resets the scroll position.
        if (abs(event.y - last_y) < 10) {
            scroll_offset_ = 0;
        }
        return true;
    }

    return false;
}

// Static state for callback-based app interface.
namespace {
    NotificationManager* g_notifications = nullptr;

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
    g_notifications = &mgr;

    // Fully qualify to avoid resolving as a recursive call to this member function.
    return aiwatchos::make_app("notifications", notif_init, notif_tick, notif_render, notif_touch);
}

}  // namespace aiwatchos
