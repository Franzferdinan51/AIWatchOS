// AIWatchOS notifications — displays incoming messages with scroll support.
// Shows notification title and body text on the 410x502 AMOLED, with touch-driven
// scrolling for multi-line content (up to ~30 lines of text at 8px/line).
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

    static App make_app();

 private:
    Hal& board_;
    std::vector<Notification> notifications_;   // most recent first
    int scroll_offset_ = 0;   // vertical pixels scrolled in the current notification
};

}  // namespace aiwatchos
