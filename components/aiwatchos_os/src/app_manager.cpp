// AppManager implementation — app registry with tick/render/touch dispatch
// and switch-next/switch-prev launcher behavior.
#include "aiwatchos/app_manager.hpp"

namespace aiwatchos {

AppManager::AppManager(Hal& board) : board_(board) {}

bool AppManager::register_app(App* app, bool launch_now) {
    if (!app || !app->id) return false;
    // Reject duplicate ids.
    for (auto* a : apps_) {
        if (a == app || (a->id && std::string(a->id) == std::string(app->id))) {
            return false;
        }
    }
    apps_.push_back(app);
    if (launch_now) {
        foreground_ = app;
        if (app->init) app->init();
    }
    return true;
}

void AppManager::unregister_app(const char* id) {
    for (size_t i = 0; i < apps_.size(); ++i) {
        if (apps_[i]->id && std::string(apps_[i]->id) == std::string(id)) {
            if (foreground_ == apps_[i]) foreground_ = nullptr;
            apps_.erase(apps_.begin() + static_cast<long>(i));
            return;
        }
    }
}

App* AppManager::find_app(const char* id) const {
    for (auto* a : apps_) {
        if (a->id && std::string(a->id) == std::string(id)) return a;
    }
    return nullptr;
}

bool AppManager::switch_to(const char* id) {
    App* target = find_app(id);
    if (!target) return false;
    if (foreground_ && foreground_->pause) foreground_->pause();
    foreground_ = target;
    if (target->init) target->init();   // idempotent: apps guard their own init state
    if (target->resume) target->resume();
    return true;
}

void AppManager::switch_next() {
    if (apps_.empty()) return;
    size_t idx = 0;
    for (size_t i = 0; i < apps_.size(); ++i) {
        if (apps_[i] == foreground_) { idx = i; break; }
    }
    switch_to(apps_[(idx + 1) % apps_.size()]->id);
}

void AppManager::switch_prev() {
    if (apps_.empty()) return;
    size_t idx = 0;
    for (size_t i = 0; i < apps_.size(); ++i) {
        if (apps_[i] == foreground_) { idx = i; break; }
    }
    switch_to(apps_[(idx + apps_.size() - 1) % apps_.size()]->id);
}

const char* AppManager::current_app_id() const {
    return foreground_ ? foreground_->id : nullptr;
}

App* AppManager::app_at_index(size_t idx) const {
    if (idx >= apps_.size()) return nullptr;
    return apps_[idx];
}

void AppManager::tick(uint32_t elapsed_ms) {
    if (foreground_ && foreground_->tick) {
        foreground_->tick(elapsed_ms);
    }
}

void AppManager::render(Framebuffer& fb) {
    if (foreground_ && foreground_->render) {
        foreground_->render(fb);
    }
}

bool AppManager::on_touch(const TouchEvent& event) {
    if (foreground_ && foreground_->on_touch) {
        return foreground_->on_touch(event);
    }
    return false;
}

}  // namespace aiwatchos
