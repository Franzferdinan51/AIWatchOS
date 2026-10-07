// AIWatchOS app manager — maintains a registry of installed apps, dispatches
// tick/render/touch events to the foreground app, and provides an app-switcher.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

#include "aiwatchos/app.hpp"
#include "aiwatchos/hal.hpp"

namespace aiwatchos {

class AppManager {
 public:
    explicit AppManager(Hal& board);

    // Register an app. Its init() is called immediately if launch_now is true,
    // otherwise on first switch to it. Returns false if the id is already registered.
    bool register_app(App* app, bool launch_now = false);

    // Remove an app from the registry (does not call any lifecycle hook).
    void unregister_app(const char* id);

    // Switch foreground to the named app; calls pause() on the old and resume()/init()
    // on the new. Returns true if found.
    bool switch_to(const char* id);

    // Cycle to the next/previous registered app (launcher behavior).
    void switch_next();
    void switch_prev();

    const char* current_app_id() const;
    App* current_app() const { return foreground_; }

    // Number of registered apps.
    size_t count() const { return apps_.size(); }

    // --- Event dispatch (called from the main loop) ---
    void tick(uint32_t elapsed_ms);
    void render(Framebuffer& fb);
    bool on_touch(const TouchEvent& event);

 private:
    Hal& board_;
    std::vector<App*> apps_;       // ordered list of registered apps
    App* foreground_ = nullptr;

    App* find_app(const char* id) const;
};

}  // namespace aiwatchos
