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
};

class NotificationManager {
 public:
    explicit NotificationManager(Hal& board);

    void render(Framebuffer& fb);
    bool on_touch(const TouchEvent& event);

    // Add a new notification (called by BLE sync stub or AI app).
    void add_notification(std::string title, std::string body);

    size_t count() const { return notifications_.size(); }

    static App make_app();

 private:
    // Wrapped-body line count for a notification at the card width.
    int body_lines(const Notification& n, bool expanded) const;

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

}  // namespace aiwatchos
