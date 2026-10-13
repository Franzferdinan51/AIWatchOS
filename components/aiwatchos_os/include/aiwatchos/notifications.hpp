// AIWatchOS notifications — Muse Gloss card list (plan 3.5).
//
// The audit's defects, closed: the old screen showed ONE notification
// (the rest were unreachable), wrapped the body one word per line, and
// stored timestamps it never rendered. Now: a scrollable stack of
// bubble cards, newest first — bell chip, title (Body SemiBold,
// ellipsis-truncated), body (Caption `dim`, wrapped by pixel width),
// and the receipt time (Micro `faint`, right). Drag scrolls the stack,
// tap expands a card's full body, swipe right dismisses it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "aiwatchos/app.hpp"
#include "aiwatchos/hal.hpp"   // for Hal, kDisplayWidth/kDisplayHeight constants

namespace aiwatchos {

struct Notification {
    std::string title;   // sender name or app label (e.g. "WhatsApp", "Email")
    std::string body;    // message content (<= 500 chars for display)
    uint64_t timestamp_ms = 0;   // when received, from PCF85063 RTC
    // Actionable entries (H10): a tap opens `action_app` instead of
    // expanding, while the entry is live. `action_id` identifies the
    // producer's object (e.g. a Hermes prompt id) for dedupe/retire.
    std::string action_app;     // empty = plain notification
    std::string action_id;
    uint64_t expires_ms = 0;    // board clock; 0 = never expires
};

class NotificationManager {
 public:
    explicit NotificationManager(Hal& board);

    void render(Framebuffer& fb);
    bool on_touch(const TouchEvent& event);

    // Add a new notification (called by BLE sync stub or AI app).
    void add_notification(std::string title, std::string body);

    size_t count() const { return notifications_.size(); }

    // Actionable entries (H10): deduped by (action_app, action_id) —
    // re-posting the same id UPDATES the entry in place (one persistent
    // entry per live object, e.g. one per Hermes prompt). expires_ms is
    // a board-clock deadline (0 = never): an expired entry stays listed
    // but renders EXPIRED and its tap no longer fires the action.
    void add_actionable(std::string title, std::string body,
                        std::string action_app, std::string action_id,
                        uint64_t expires_ms);
    bool retire_action(const std::string& action_app,
                       const std::string& action_id);
    bool has_action(const std::string& action_app,
                    const std::string& action_id) const;
    const Notification& at(size_t i) const { return notifications_[i]; }
    bool expired(const Notification& n) const {
        return n.expires_ms != 0 && board_.now_ms() >= n.expires_ms;
    }

    // Tap on a live actionable entry calls this (app_main wires it to
    // AppManager::switch_to). Plain entries keep tap-to-expand.
    using ActionHandler = void (*)(const char* action_app, void* ctx);
    void set_action_handler(ActionHandler handler, void* ctx) {
        action_handler_ = handler;
        action_ctx_ = ctx;
    }

    static App make_app();

 private:
    // Wrapped-body line count for a notification at the card width.
    int body_lines(const Notification& n, bool expanded) const;

    ActionHandler action_handler_ = nullptr;
    void* action_ctx_ = nullptr;

    Hal& board_;
    std::vector<Notification> notifications_;   // oldest first; UI shows newest first
    int scroll_offset_ = 0;   // vertical pixels the stack is scrolled
    int expanded_ = -1;       // notifications_ index shown in full (-1 = none)

    // Touch tracking for tap / drag-scroll / swipe-dismiss.
    int press_x_ = 0;
    int press_y_ = 0;
    int last_y_ = 0;
    int press_card_ = -1;     // notifications_ index under the press
    bool drag_moved_ = false;
};

// The manager behind the Notifications app (nullptr until
// NotificationManager::make_app ran) — the seam producers post
// through (H10).
NotificationManager* active_notifications();
// Test seam: point the active manager at a test-owned instance.
void set_active_notifications(NotificationManager* mgr);

}  // namespace aiwatchos
