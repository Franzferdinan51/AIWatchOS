// Hermes Gadget app adapter — a real Hermes Gadget Protocol v1 client
// (hermes_proto.cpp) presented as an AIWatchOS OS app.
//
// The adapter owns the single HermesClient instance (heap-allocated once —
// PSRAM on device, so its multi-KB view buffers never touch internal SRAM)
// and the render/touch/health surface. The link layer (hermes_link.cpp)
// owns WiFi + the WebSocket and feeds frames into the client from its own
// task, so the session stays alive while other apps are foregrounded.
#include "aiwatchos_hermes.hpp"
#include "aiwatchos/notifications.hpp"  // H10 bridge target
#include "hermes_internal.hpp"

#include "aiwatchos/colors.hpp"
#include "aiwatchos/font.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifdef __ESPRESSIF_IDF__
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#endif

namespace aiwatchos_hermes {
namespace {

// Hermes identity (DESIGN_TOKENS section 9): the Muse-blue OS with
// a gold dialect — same geometry, gold replaces blue in chrome, and
// its own text ramp. State colours stay state colours.
namespace hc = aiwatchos::hcolors;
constexpr uint16_t kHermesBg = hc::kBg;         // deep blue-black
constexpr uint16_t kTextColor = hc::kText;      // warm white
constexpr uint16_t kDimColor = hc::kDim;        // slate body text
constexpr uint16_t kStatusColor = hc::kDim;     // status lines
constexpr uint16_t kAmberColor = hc::kGold;     // notices / pairing
constexpr uint16_t kGreenColor = hc::kYes;      // YES / online accents
constexpr uint16_t kRedColor = hc::kNo;         // NO / errors

aiwatchos::Hal* g_hal = nullptr;
HermesClient* g_client = nullptr;
bool g_initialized = false;

#ifdef __ESPRESSIF_IDF__
SemaphoreHandle_t g_lock = nullptr;
struct LockGuard {
    LockGuard() { if (g_lock) xSemaphoreTake(g_lock, portMAX_DELAY); }
    ~LockGuard() { if (g_lock) xSemaphoreGive(g_lock); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
};
#else
struct LockGuard { LockGuard() = default; };
#endif

// --- outbound capture (host tests only; device sends via the link layer) --
#ifndef __ESPRESSIF_IDF__
struct CapturedFrame { bool binary; std::vector<uint8_t> bytes; };
std::vector<CapturedFrame> g_outbound;
#endif

bool sink_send_text(void*, const char* json, size_t len) {
#ifdef __ESPRESSIF_IDF__
    return hermes_link_send_text(json, len);
#else
    CapturedFrame f;
    f.binary = false;
    f.bytes.assign(json, json + len);
    g_outbound.push_back(std::move(f));
    return true;
#endif
}

bool sink_send_binary(void*, const uint8_t* data, size_t len) {
#ifdef __ESPRESSIF_IDF__
    return hermes_link_send_binary(data, len);
#else
    CapturedFrame f;
    f.binary = true;
    f.bytes.assign(data, data + len);
    g_outbound.push_back(std::move(f));
    return true;
#endif
}

// --- audio sinks: spoken replies play through the OS HAL (Board on device,
// TestHal in host tests, which captures the samples) -------------------------
void sink_audio_begin(void*, uint32_t rate) {
    if (g_hal) g_hal->audio_start_playback(rate);
}
void sink_audio_write(void*, const int16_t* samples, size_t count) {
    if (g_hal) g_hal->audio_write(samples, count);
}
void sink_audio_end(void*) {
    if (g_hal) g_hal->audio_stop_playback();
}
void sink_audio_abort(void*) {
    if (g_hal) g_hal->audio_stop_playback();
}

// --- OTA / identity / memory sinks delegate to the link layer --------------
bool sink_ota_begin(void*, uint32_t size) { return hermes_link_ota_begin(size); }
bool sink_ota_write(void*, const uint8_t* data, size_t len) { return hermes_link_ota_write(data, len); }
bool sink_ota_finish(void*) { return hermes_link_ota_finish(); }
void sink_ota_abort(void*) { hermes_link_ota_abort(); }
void sink_fill_random(void*, uint8_t* out, size_t len) { hermes_link_fill_random(out, len); }
void* sink_alloc_big(void*, size_t len) { return hermes_link_alloc_big(len); }
void sink_free_big(void*, void* ptr) { hermes_link_free_big(ptr); }

uint64_t now_ms() { return g_hal ? g_hal->now_ms() : 0; }

HermesClient* ensure_client() {
    if (g_client) return g_client;
#ifdef __ESPRESSIF_IDF__
    void* mem = heap_caps_malloc(sizeof(HermesClient), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) return nullptr;   // fail closed: no client, app shows OFFLINE
    g_client = new (mem) HermesClient();
#else
    g_client = new HermesClient();
#endif
    HermesSinks sinks{};
    sinks.send_text = &sink_send_text;
    sinks.send_binary = &sink_send_binary;
    sinks.audio_begin = &sink_audio_begin;
    sinks.audio_write = &sink_audio_write;
    sinks.audio_end = &sink_audio_end;
    sinks.audio_abort = &sink_audio_abort;
    sinks.ota_begin = &sink_ota_begin;
    sinks.ota_write = &sink_ota_write;
    sinks.ota_finish = &sink_ota_finish;
    sinks.ota_abort = &sink_ota_abort;
    sinks.fill_random = &sink_fill_random;
    sinks.alloc_big = &sink_alloc_big;
    sinks.free_big = &sink_free_big;
    HermesConfig cfg{};
#ifdef __ESPRESSIF_IDF__
#ifdef CONFIG_HERMES_DEVICE_NAME
    std::snprintf(cfg.name, sizeof(cfg.name), "%s", CONFIG_HERMES_DEVICE_NAME);
#endif
#endif
    g_client->init(sinks, cfg);
    // Identity: stored key wins; otherwise generate once and persist it
    // (device). Host builds get whatever the stub provides — tests install
    // their own key with hermes_test_reset().
    uint8_t key[32];
    if (hermes_link_load_key(key)) {
        g_client->set_key(key);
    } else {
        hermes_link_fill_random(key, sizeof(key));
        g_client->set_key(key);
        hermes_link_store_key(key);
    }
    return g_client;
}

const char* link_word(HermesLinkState s) {
    switch (s) {
        case HermesLinkState::Offline: return "OFFLINE";
        case HermesLinkState::Connecting: return "CONNECTING";
        case HermesLinkState::Pairing: return "PAIRING";
        case HermesLinkState::Linked: return "ONLINE";
        case HermesLinkState::AuthFailed: return "AUTH FAIL";
    }
    return "OFFLINE";
}

// --- Text rendering --------------------------------------------------
// All Hermes text goes through the baked AA faces (aiwatchos/font.hpp)
// — the same glyphs as every other app. The helpers keep the render
// code's historical call shapes: the old 5x7 `scale` argument now
// selects a face (2 = Body, 3 = Title, 4/6 = Pair) and wrapping is
// measured in pixels, not character cells.

aiwatchos::FontFace face_for_scale(int scale) {
    switch (scale) {
        case 2: return aiwatchos::FontFace::kBody;
        case 3: return aiwatchos::FontFace::kTitle;
        case 4:
        case 6: return aiwatchos::FontFace::kPair;
        default: return aiwatchos::FontFace::kBody;
    }
}

void draw_text(aiwatchos::Framebuffer& fb, int x, int y, const char* s,
               int scale, uint16_t color) {
    aiwatchos::draw_text_aa(fb, x, y, s, face_for_scale(scale), color);
}

int text_w(const char* s, int scale) {
    return aiwatchos::text_width_aa(s, face_for_scale(scale));
}

// Word-wrapped text block (pixel-measured); returns the y just past
// the last drawn line.
int draw_wrapped(aiwatchos::Framebuffer& fb, int x, int y, int max_w,
                 const char* s, int scale, uint16_t color, int max_lines) {
    if (!s) return y;
    const aiwatchos::FontFace face = face_for_scale(scale);
    const int line_step = aiwatchos::font_line_height(face) + 2;
    int lines = 0;
    const char* p = s;
    while (*p && lines < max_lines) {
        // Greedy word wrap measured in pixels.
        int best = 0;        // chars that fit (at a word boundary)
        int len = 0;         // scan cursor
        int last_space = -1;
        while (p[len]) {
            if (p[len] == '\n') break;
            if (p[len] == ' ') last_space = len;
            char probe[176];
            const int n = len + 1;
            if (n >= static_cast<int>(sizeof(probe))) break;
            std::memcpy(probe, p, static_cast<size_t>(n));
            probe[n] = '\0';
            if (aiwatchos::text_width_aa(probe, face) > max_w) break;
            ++len;
            if (last_space == len - 1) best = len - 1;
        }
        int take;
        if (!p[len]) {
            take = len;                     // the rest fits / end of text
        } else if (p[len] == '\n') {
            take = len;
        } else if (last_space > 0) {
            take = last_space;              // break at the last space
        } else {
            take = len > 0 ? len : 1;       // one overlong word: hard cut
        }
        if (take <= 0) break;
        char line[176];
        if (take >= static_cast<int>(sizeof(line))) {
            take = static_cast<int>(sizeof(line)) - 1;
        }
        std::memcpy(line, p, static_cast<size_t>(take));
        line[take] = '\0';
        aiwatchos::draw_text_aa(fb, x, y, line, face, color);
        y += line_step;
        ++lines;
        p += take;
        while (*p == ' ' || *p == '\n') ++p;
    }
    return y;
}

// Count the lines draw_wrapped would produce (for sizing bubbles).
int wrapped_line_count(const char* s, int max_w, int scale, int max_lines) {
    if (!s) return 0;
    const aiwatchos::FontFace face = face_for_scale(scale);
    int lines = 0;
    const char* p = s;
    while (*p && lines < max_lines) {
        int len = 0, last_space = -1;
        while (p[len]) {
            if (p[len] == '\n') break;
            if (p[len] == ' ') last_space = len;
            char probe[176];
            const int n = len + 1;
            if (n >= static_cast<int>(sizeof(probe))) break;
            std::memcpy(probe, p, static_cast<size_t>(n));
            probe[n] = '\0';
            if (aiwatchos::text_width_aa(probe, face) > max_w) break;
            ++len;
        }
        int take;
        if (!p[len] || p[len] == '\n') take = len;
        else if (last_space > 0) take = last_space;
        else take = len > 0 ? len : 1;
        if (take <= 0) break;
        ++lines;
        p += take;
        while (*p == ' ' || *p == '\n') ++p;
    }
    return lines;
}

}  // namespace

// ---------------------------------------------------------------------------
// Health line / uptime (public, unchanged semantics + REAL link state)
// ---------------------------------------------------------------------------

char* hermes_uptime_str(uint64_t now_ms, char* out, size_t len) {
    if (!out || len == 0) return out;
    uint64_t total_s = now_ms / 1000;
    if (total_s < 3600) {
        std::snprintf(out, len, "%02u:%02u",
                      static_cast<unsigned>((total_s / 60) % 60),
                      static_cast<unsigned>(total_s % 60));
    } else {
        std::snprintf(out, len, "%02u:%02u",
                      static_cast<unsigned>((total_s / 3600) % 100),
                      static_cast<unsigned>((total_s / 60) % 60));
    }
    out[len - 1] = '\0';
    return out;
}

std::string hermes_health_line() {
    char up[8] = {};
    uint64_t now = g_hal ? g_hal->now_ms() : 0;
    hermes_uptime_str(now, up, sizeof(up));
    HermesLinkState st = HermesLinkState::Offline;
    char srv[24] = {};
    char ses[40] = {};
    if (g_client) {
        LockGuard lock;
        st = g_client->state();
        std::snprintf(srv, sizeof(srv), "%s", g_client->view().server);
        std::snprintf(ses, sizeof(ses), "%s", g_client->view().session);
    }
    char line[64] = {};
    if (g_hal) {
        aiwatchos::PowerStatus ps = g_hal->read_power();
        if (ps.battery_percent <= 100) {
            std::snprintf(line, sizeof(line), "BAT %u%%%s UP %s LINK %s",
                          ps.battery_percent, ps.charging ? " CHG" : "", up, link_word(st));
        } else {
            std::snprintf(line, sizeof(line), "BAT -- UP %s LINK %s", up, link_word(st));
        }
    } else {
        std::snprintf(line, sizeof(line), "BAT -- UP %s LINK %s", up, link_word(st));
    }
    // Diagnostics (buddy-harvest): the hub's server identity + our
    // session id, as welcomed. Session truncated to its first 8 chars.
    if (srv[0]) {
        char srv_up[24];
        size_t i = 0;
        for (; i < sizeof(srv_up) - 1 && srv[i]; ++i) {
            const char c = srv[i];
            srv_up[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
        }
        srv_up[i] = '\0';
        // The endpoint in use rides along (LAN at home, REMOTE via the
        // hub's Tailscale Funnel away) so a dead link says where it died.
        char tail[56];
        if (ses[0]) {
            std::snprintf(tail, sizeof(tail), " SRV %.16s SES %.8s VIA %s", srv_up, ses,
                          hermes_link_endpoint_tag());
        } else {
            std::snprintf(tail, sizeof(tail), " SRV %.16s VIA %s", srv_up,
                          hermes_link_endpoint_tag());
        }
        const size_t used = std::strlen(line);
        if (used + 1 < sizeof(line)) {
            std::strncat(line, tail, sizeof(line) - used - 1);
        }
    }
    return std::string(line);
}

// ---------------------------------------------------------------------------
// Live client API
// ---------------------------------------------------------------------------

HermesLinkState hermes_link_state() {
    if (!g_client) return HermesLinkState::Offline;
    LockGuard lock;
    return g_client->state();
}

bool hermes_paired() {
    if (!g_client) return false;
    LockGuard lock;
    return g_client->paired();
}

namespace {
// Borrowed-view access: the view lives inside the client (PSRAM on
// device). Callers get a reference, never a copy — the struct is ~35 KB
// of text buffers (H1 ring included) and the OS main task stack cannot
// hold one. On device the
// link task may update it concurrently; render/touch hold the client lock
// while reading, API callers treat the reference as a transient snapshot.
HermesView* g_empty_view = nullptr;   // heap fallback when no client exists
}  // namespace

const HermesView& hermes_view() {
    if (g_client) return g_client->view();
    if (!g_empty_view) g_empty_view = new HermesView();
    return *g_empty_view;
}

const char* hermes_pairing_code() {
    return hermes_view().pairing_code;
}

const char* hermes_pairing_command() {
    return hermes_view().pairing_cmd;
}

bool hermes_send_text(const char* text) {
    if (!ensure_client()) return false;
    LockGuard lock;
    const bool ok = g_client->send_chat_text(text);
    // Buddy parity: sending (or queueing) pokes the reconnect loop out
    // of its backoff instead of waiting it out.
    if (ok) hermes_link_wake_reconnect();
    return ok;
}

bool hermes_answer_prompt(bool yes) {
    if (!g_client) return false;
    LockGuard lock;
    return g_client->answer_prompt(yes);
}

bool hermes_new_session() {
    if (!g_client) return false;
    LockGuard lock;
    const bool ok = g_client->new_session(now_ms());
    // The persisted conversation pair belongs to the old session.
    if (ok) hermes_link_clear_conversation();
    return ok;
}

// Buddy-harvest device-key reset (the phone's Settings button, mapped
// to the watch): erase the stored identity, mint + store a fresh key,
// and clear the client's AuthFailed latch so the link reconnects and
// re-enrolls — the hub will issue a new pairing code. Triggered by the
// BOOT long-hold on the AUTH FAIL screen (see hermes_tick).
bool hermes_forget_device() {
    if (!ensure_client()) return false;
    LockGuard lock;
    hermes_link_erase_key();
    uint8_t key[32];
    hermes_link_fill_random(key, sizeof(key));
    g_client->set_key(key);
    hermes_link_store_key(key);
    g_client->forget();
    hermes_link_wake_reconnect();
    return true;
}

// --- H10: Hermes events -> OS Notifications ---------------------------------
// Runs inside hermes_pump (the link task's cadence on device) so events
// post even while another app is foregrounded. Edge-detected against
// the client view: a prompt posts ONE actionable entry (deduped by
// prompt id in the manager), a card/notice posts once per activation.
bool g_bridge_prompt_posted = false;
char g_bridge_prompt_id[32] = {};
uint64_t g_bridge_prompt_since_ms = 0;
bool g_bridge_card_on = false;
uint64_t g_bridge_card_until_ms = 0;
char g_bridge_card_title[96] = {};
char g_bridge_notice[160] = {};
uint64_t g_bridge_notice_until_ms = 0;
// Buddy-harvest: completed-reply notifications + persistence sync.
bool g_bridge_reply_busy = false;
char g_bridge_reply_sig[80] = {};   // first chars of the last notified reply
uint64_t g_fg_tick_ms = 0;          // last hermes_tick (app foregrounded)
bool g_fg_seen = false;
uint32_t g_outbox_saved_version = 0;
bool g_conv_prev_busy = false;

void hermes_notify_bridge(const HermesView& v, uint64_t now) {
    aiwatchos::NotificationManager* nm = aiwatchos::active_notifications();
    if (!nm) return;

    // Prompt: post/refresh while active; retire when it closes BEFORE
    // its TTL (answered or dismissed). If the TTL lapses first, the
    // entry stays — its deadline makes the manager render it EXPIRED,
    // never actionable.
    if (v.prompt_active && v.prompt_id[0]) {
        const bool ttl_lapsed = v.prompt_ttl_s > 0 &&
            now - v.prompt_since_ms >=
                static_cast<uint64_t>(v.prompt_ttl_s) * 1000ULL;
        if (ttl_lapsed) {
            g_bridge_prompt_posted = false;   // leave the entry, expired
        } else if (!g_bridge_prompt_posted ||
                   std::strcmp(g_bridge_prompt_id, v.prompt_id) != 0 ||
                   g_bridge_prompt_since_ms != v.prompt_since_ms) {
            const uint64_t expires = v.prompt_ttl_s > 0
                ? v.prompt_since_ms +
                      static_cast<uint64_t>(v.prompt_ttl_s) * 1000ULL
                : 0;
            nm->add_actionable(
                v.prompt_title[0] ? v.prompt_title : "Hermes prompt",
                v.prompt_text[0] ? v.prompt_text
                                 : "Hermes needs your answer.",
                "hermes", v.prompt_id, expires);
            std::snprintf(g_bridge_prompt_id, sizeof(g_bridge_prompt_id),
                          "%s", v.prompt_id);
            g_bridge_prompt_since_ms = v.prompt_since_ms;
            g_bridge_prompt_posted = true;
        }
    } else if (g_bridge_prompt_posted) {
        nm->retire_action("hermes", g_bridge_prompt_id);
        g_bridge_prompt_posted = false;
        g_bridge_prompt_id[0] = '\0';
        g_bridge_prompt_since_ms = 0;
    }

    // Display / image card: one plain entry per activation identity.
    const bool card_on = v.card_active || v.image_active;
    const uint64_t card_until =
        v.card_active ? v.card_until_ms : v.image_until_ms;
    if (card_on) {
        if (!g_bridge_card_on || card_until != g_bridge_card_until_ms ||
            std::strcmp(g_bridge_card_title, v.card_title) != 0) {
            nm->add_notification(
                v.card_title[0] ? v.card_title : "Hermes",
                v.card_body[0] ? v.card_body : v.status);
            std::snprintf(g_bridge_card_title, sizeof(g_bridge_card_title),
                          "%s", v.card_title);
            g_bridge_card_until_ms = card_until;
        }
        g_bridge_card_on = true;
    } else {
        g_bridge_card_on = false;
        g_bridge_card_until_ms = 0;
    }

    // Completed reply while the Hermes app is backgrounded
    // (buddy-harvest): post it to OS Notifications like the phone does.
    // Edge = busy fall with reply text; a fresh turn re-arms (the
    // signature clears on the busy rise) so an identical later reply
    // still notifies. Foreground = hermes_tick ran within 600 ms.
    const bool fg = g_fg_seen && now >= g_fg_tick_ms && now - g_fg_tick_ms <= 600;
    if (v.busy) {
        if (!g_bridge_reply_busy) g_bridge_reply_sig[0] = '\0';
    } else if (g_bridge_reply_busy && v.reply[0] && !fg &&
               std::strncmp(v.reply, g_bridge_reply_sig,
                            sizeof(g_bridge_reply_sig) - 1) != 0) {
        nm->add_notification("Hermes", v.reply);
        std::snprintf(g_bridge_reply_sig, sizeof(g_bridge_reply_sig),
                      "%s", v.reply);
    }
    g_bridge_reply_busy = v.busy;

    // Notice banner: one entry per (text, deadline).
    if (v.notice[0] && v.notice_until_ms) {
        if (std::strcmp(g_bridge_notice, v.notice) != 0 ||
            g_bridge_notice_until_ms != v.notice_until_ms) {
            nm->add_notification("Hermes", v.notice);
            std::snprintf(g_bridge_notice, sizeof(g_bridge_notice), "%s",
                          v.notice);
            g_bridge_notice_until_ms = v.notice_until_ms;
        }
    }
}

void hermes_pump(uint64_t now_ms) {
    if (!g_client) return;
    LockGuard lock;
    // H10 bridge BEFORE tick(): tick() clears an expired prompt from
    // the view, and the bridge must see it first to leave its entry in
    // place as EXPIRED rather than retiring it as if answered.
    hermes_notify_bridge(g_client->view(), now_ms);
    g_client->tick(now_ms);

    // Buddy-harvest persistence sync (same lock, link-task cadence).
    // Outbox: rewrite the stored ring whenever the queue mutated —
    // enqueue, eviction, and the post-pairing flush all bump the
    // version, so the NVS copy always mirrors the live ring.
    if (g_client->outbox_version() != g_outbox_saved_version) {
        const char* texts[4];  // kOutboxMax, mirrored
        const int n = g_client->outbox_count();
        for (int i = 0; i < n; ++i) texts[i] = g_client->outbox_at(i);
        hermes_link_save_outbox(texts, n);
        g_outbox_saved_version = g_client->outbox_version();
    }
    // Conversation: persist the transcript+reply pair when a turn
    // settles (busy fall) — one NVS write per turn, never per delta.
    const HermesView& v = g_client->view();
    if (g_conv_prev_busy && !v.busy) {
        hermes_link_save_conversation(v.transcript, v.reply);
    }
    g_conv_prev_busy = v.busy;
}

// ---------------------------------------------------------------------------
// Link-layer entry points (called from hermes_link.cpp's task on device)
// ---------------------------------------------------------------------------

void hermes_core_transport_up() {
    if (!ensure_client()) return;
    LockGuard lock;
    g_client->on_transport_open(now_ms());
}

void hermes_core_transport_down() {
    if (!g_client) return;
    LockGuard lock;
    g_client->on_transport_closed(now_ms());
}

void hermes_core_transport_text(const char* json, size_t len) {
    if (!g_client) return;
    LockGuard lock;
    g_client->on_text(json, len, now_ms());
}

void hermes_core_transport_binary(const uint8_t* data, size_t len) {
    if (!g_client) return;
    LockGuard lock;
    g_client->on_binary(data, len, now_ms());
}

void hermes_core_pump() { hermes_pump(now_ms()); }

bool hermes_core_wants_reconnect() {
    if (!g_client) return false;
    LockGuard lock;
    return g_client->wants_reconnect();
}

bool hermes_core_talk_active() {
    if (!g_client) return false;
    LockGuard lock;
    return g_client->talk_active();
}

void hermes_core_talk_feed(const int16_t* pcm, size_t frames) {
    if (!g_client) return;
    LockGuard lock;
    g_client->talk_feed(pcm, frames);
}

void hermes_core_set_battery(int pct, int charging) {
    if (!g_client) return;
    LockGuard lock;
    g_client->set_battery(pct, charging, now_ms());
}

bool hermes_core_ota_reboot_pending() {
    if (!g_client) return false;
    LockGuard lock;
    return g_client->ota_reboot_pending();
}

void hermes_core_ota_reboot_consumed() {
    if (!g_client) return;
    LockGuard lock;
    g_client->ota_reboot_consumed();
}

// ---------------------------------------------------------------------------
// Buddy-harvest: boot restore of the persisted Hermes state
// ---------------------------------------------------------------------------
// One-time PSRAM scratch for the stored blobs (never DRAM): the saved
// conversation pair and outbox ring are re-injected into the fresh
// client exactly as if the reboot had not happened.
char* g_scratch_t = nullptr;
char* g_scratch_r = nullptr;
char (*g_scratch_q)[512] = nullptr;

bool hermes_scratch_ok() {
    if (!g_scratch_t) {
        g_scratch_t = static_cast<char*>(sink_alloc_big(nullptr, 512));
    }
    if (!g_scratch_r) {
        g_scratch_r = static_cast<char*>(sink_alloc_big(nullptr, 4096));
    }
    if (!g_scratch_q) {
        g_scratch_q = static_cast<char(*)[512]>(sink_alloc_big(nullptr, 4 * 512));
    }
    return g_scratch_t && g_scratch_r && g_scratch_q;
}

void hermes_restore_persisted() {
    if (!g_client || !hermes_scratch_ok()) return;
    LockGuard lock;
    if (hermes_link_load_conversation(g_scratch_t, 512, g_scratch_r, 4096)) {
        g_client->restore_conversation(g_scratch_t, g_scratch_r);
    }
    const int n = hermes_link_load_outbox(g_scratch_q, 4);
    for (int i = 0; i < n; ++i) {
        if (g_scratch_q[i][0]) g_client->send_chat_text(g_scratch_q[i]);
    }
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------

void hermes_init() {
    if (g_initialized) return;
    g_initialized = true;
#ifdef __ESPRESSIF_IDF__
    if (!g_lock) g_lock = xSemaphoreCreateMutex();
#endif
    ensure_client();
    hermes_link_start();   // device: WS link task (WiFi owned by aiwatchos_net); host: stub
    hermes_restore_persisted();
}

// H5 gesture state: BOOT held in the Hermes app. Hint at 600 ms,
// session.new fires once at 2000 ms (the reference firmware's
// long-press thresholds).
bool g_boot_held = false;
bool g_boot_fired = false;
uint64_t g_boot_since_ms = 0;
uint32_t g_boot_hint_ms = 0;

void hermes_tick(uint32_t elapsed_ms) {
    (void)elapsed_ms;
    const uint64_t now = now_ms();
    // Foreground tracking for the notification bridge, plus the buddy
    // wakeup: returning to the app after a gap pokes the link layer
    // out of its reconnect backoff.
    if (g_fg_seen && now > g_fg_tick_ms && now - g_fg_tick_ms > 1500) {
        hermes_link_wake_reconnect();
    }
    g_fg_seen = true;
    g_fg_tick_ms = now;
    if (g_hal) {
        const bool down = (g_hal->read_buttons() & 1u) != 0;   // BOOT
        if (down) {
            if (!g_boot_held) {
                g_boot_held = true;
                g_boot_fired = false;
                g_boot_since_ms = now;
            }
            g_boot_hint_ms = static_cast<uint32_t>(now - g_boot_since_ms);
            if (!g_boot_fired && g_boot_hint_ms >= 2000) {
                g_boot_fired = true;
                // Context-aware (buddy-harvest): on the AUTH FAIL
                // screen the same hold forgets the rejected identity
                // and re-enrolls instead of starting a session.
                if (g_client &&
                    g_client->state() == HermesLinkState::AuthFailed) {
                    hermes_forget_device();
                } else {
                    hermes_new_session();
                }
            }
        } else {
            g_boot_held = false;
            g_boot_hint_ms = 0;
        }
    }
    hermes_pump(now);
}

void hermes_render(aiwatchos::Framebuffer& fb) {
    fb.fill(kHermesBg);
    // Gold identity title (tokens section 9). No header band — the OS
    // status zone lives above and is repainted by the shell.
    draw_text(fb, 16, 46, "Hermes", 3, kAmberColor);

    // The view is read in place under the client lock — never copied. It
    // is ~35 KB of text buffers and this runs on the OS main task's stack.
    LockGuard lock;
    const HermesView& v = hermes_view();
    const uint8_t* image_px = nullptr;
    int image_w = 0, image_h = 0;
    if (g_client && v.image_active) {
        image_px = g_client->image_pixels();
        image_w = v.image_w;
        image_h = v.image_h;
    }

    // State headline.
    char headline[48];
    std::snprintf(headline, sizeof(headline), "LINK %s", link_word(v.link));
    draw_text(fb, 16, 88, headline, 2,
              v.link == HermesLinkState::Linked ? hc::kOnline
              : v.link == HermesLinkState::AuthFailed
                    ? aiwatchos::colors::kDanger
                    : kAmberColor);

    // Device-health battery bar (tokens section 8 geometry, gold
    // dialect): r-pill outline faint, state-coloured fill, percent in
    // Body at the right. The old white charging square is gone — the
    // fill colour carries the charging state (kOk), red under 20%.
    if (g_hal) {
        aiwatchos::PowerStatus ps = g_hal->read_power();
        constexpr int kBarX = 16, kBarY = 120, kBarW = 200, kBarH = 20;
        fb.fill_round_rect(kBarX, kBarY, kBarW, kBarH, 10, hc::kFaint);
        fb.fill_round_rect(kBarX + 2, kBarY + 2, kBarW - 4, kBarH - 4, 8,
                           hc::kBar);
        if (ps.battery_percent <= 100) {
            const uint16_t fill_color =
                ps.charging ? aiwatchos::colors::kOk
                : ps.battery_percent < 20 ? aiwatchos::colors::kDanger
                                           : kAmberColor;
            const int fill = (kBarW - 4) * ps.battery_percent / 100;
            if (fill > 0) {
                fb.fill_round_rect(kBarX + 2, kBarY + 2, fill, kBarH - 4, 8,
                                   fill_color);
            }
            char pct_buf[8];
            std::snprintf(pct_buf, sizeof(pct_buf), "%u%%",
                          static_cast<unsigned>(ps.battery_percent));
            draw_text(fb, kBarX + kBarW + 12, kBarY - 1, pct_buf, 2,
                      kTextColor);
        }
        // Micro row: uptime left, device id right.
        char up_buf[16];
        hermes_uptime_str(now_ms(), up_buf, sizeof(up_buf));
        char up_line[48];
        if (v.queued) {
            std::snprintf(up_line, sizeof(up_line), "UP %s - %u QUEUED",
                          up_buf, v.queued);
        } else {
            std::snprintf(up_line, sizeof(up_line), "UP %s", up_buf);
        }
        aiwatchos::draw_text_aa(fb, 16, 146, up_line,
                                aiwatchos::FontFace::kMicro, kDimColor);
        if (g_client) {
            aiwatchos::draw_text_aa_right(
                fb, aiwatchos::kDisplayWidth - 16, 146,
                g_client->device_id(), aiwatchos::FontFace::kMicro,
                kDimColor);
        }
    }

    const int kContentY = 170;
    const int kContentW = aiwatchos::kDisplayWidth - 32;

    if (v.link == HermesLinkState::AuthFailed) {
        draw_wrapped(fb, 16, kContentY, kContentW, v.error, 2, aiwatchos::colors::kDanger, 3);
        draw_wrapped(fb, 16, kContentY + 60, kContentW,
                     "HOLD BOOT 2S TO FORGET THIS DEVICE AND RE-ENROLL WITH A FRESH KEY. (OR RUN HERMES GADGET FORGET ON THE HOST.)",
                     2, kDimColor, 8);
    } else if (v.prompt_active) {
        draw_wrapped(fb, 16, kContentY, kContentW, v.prompt_title, 3, kAmberColor, 2);
        draw_wrapped(fb, 16, kContentY + 48, kContentW, v.prompt_text, 2, kTextColor, 8);
        // YES / NO targets: gloss bubbles (state colours are allowed
        // here — they ARE the state). Touch zones unchanged.
        aiwatchos::BubbleStyle yes_style;
        yes_style.top = aiwatchos::blend565(hc::kYes, aiwatchos::colors::kWhite, 70);
        yes_style.mid = hc::kYes;
        yes_style.bot = aiwatchos::blend565(hc::kYes, aiwatchos::colors::kBlack, 70);
        yes_style.border_alpha = 100;
        yes_style.shadow = nullptr;
        aiwatchos::BubbleStyle no_style = yes_style;
        no_style.top = aiwatchos::blend565(hc::kNo, aiwatchos::colors::kWhite, 70);
        no_style.mid = hc::kNo;
        no_style.bot = aiwatchos::blend565(hc::kNo, aiwatchos::colors::kBlack, 70);
        fb.draw_bubble(16, 380, 181, 60, 16, yes_style);
        fb.draw_bubble(213, 380, 181, 60, 16, no_style);
        const int yes_w = text_w("YES", 4);
        const int no_w = text_w("NO", 4);
        draw_text(fb, 16 + (181 - yes_w) / 2, 380 + 10, "YES", 4, kHermesBg);
        draw_text(fb, 213 + (181 - no_w) / 2, 380 + 10, "NO", 4, kHermesBg);
    } else if (v.link == HermesLinkState::Pairing && v.pairing_code[0]) {
        draw_text(fb, 16, kContentY, "PAIRING CODE", 2, kStatusColor);
        int code_w = text_w(v.pairing_code, 6);
        draw_text(fb, (aiwatchos::kDisplayWidth - code_w) / 2, kContentY + 24,
                  v.pairing_code, 6, kAmberColor);
        draw_wrapped(fb, 16, kContentY + 76, kContentW,
                     "APPROVE ON THE HERMES HOST:", 2, kDimColor, 2);
        char fallback_cmd[64];
        const char* cmd = v.pairing_cmd;
        if (!cmd[0]) {
            std::snprintf(fallback_cmd, sizeof(fallback_cmd),
                          "HERMES PAIRING APPROVE GADGET %s", v.pairing_code);
            cmd = fallback_cmd;
        }
        draw_wrapped(fb, 16, kContentY + 104, kContentW, cmd,
                     2, kTextColor, 6);
    } else if (v.card_active) {
        // Card surface: a gold-dialect dialogue bubble sized to the
        // wrapped title + body.
        const int title_lines = wrapped_line_count(v.card_title, kContentW, 3, 2);
        const int body_lines = wrapped_line_count(v.card_body, kContentW, 2, 12);
        const int card_h = 16 + title_lines * 36 + 8 + body_lines * 24 + 12;
        aiwatchos::BubbleStyle card_style = aiwatchos::bubble_style();
        card_style.top = hc::kBarTop;
        card_style.mid = hc::kBar;
        card_style.bot = hc::kBarBot;
        card_style.shadow = nullptr;
        fb.draw_bubble(8, kContentY - 8, aiwatchos::kDisplayWidth - 16,
                       card_h, 16, card_style);
        draw_wrapped(fb, 16, kContentY, kContentW, v.card_title, 3, kTextColor, 2);
        draw_wrapped(fb, 16, kContentY + 48, kContentW, v.card_body, 2, kDimColor, 12);
    } else if (v.image_active && image_px && image_w > 0 && image_h > 0) {
        // Nearest-neighbour fit into the content area, aspect preserved.
        int max_w = kContentW, max_h = 280;
        int dw = max_w, dh = image_h * max_w / image_w;
        if (dh > max_h) { dh = max_h; dw = image_w * max_h / image_h; }
        int ox = (aiwatchos::kDisplayWidth - dw) / 2, oy = kContentY;
        for (int y = 0; y < dh; ++y) {
            int sy = y * image_h / dh;
            const uint8_t* row = image_px + static_cast<size_t>(sy) * image_w * 2;
            for (int x = 0; x < dw; ++x) {
                int sx = x * image_w / dw;
                uint16_t px = static_cast<uint16_t>(row[sx * 2] | (row[sx * 2 + 1] << 8));
                fb.put_pixel(ox + x, oy + y, px);
            }
        }
    } else {
        // Conversation.
        int y = kContentY;
        if (v.ota_active) {
            char ota_line[48];
            unsigned pct = v.ota_size ? v.ota_received * 100 / v.ota_size : 0;
            std::snprintf(ota_line, sizeof(ota_line), "UPDATING %u%%", pct);
            draw_text(fb, 16, y, ota_line, 2, kAmberColor);
            y += 28;
        }
        if (v.busy || v.status[0]) {
            char status_line[176];
            std::snprintf(status_line, sizeof(status_line), "%s",
                          v.status[0] ? v.status : "THINKING");
            y = draw_wrapped(fb, 16, y, kContentW, status_line, 2, kAmberColor, 2);
            y += 6;
        }
        // H1: completed exchanges as Hermes-dialect bubbles above the
        // current pair (gold bar surface, like the display card). Each
        // bubble: an HH:MM stamp (Micro, gold) from the turn end, the
        // heard transcript as one dim "YOU: ..." line, and the reply in
        // up to 2 lines. Only the newest bubbles that fit above y=318
        // draw; the current pair keeps the rest of the screen.
        if (v.history_count > 0) {
            const int kHistBottom = 318;
            int first = v.history_count;
            {
                int yy = y;
                for (int i = v.history_count - 1; i >= 0; --i) {
                    const HermesExchange& ex = v.history[i];
                    int h = 14 + (ex.end_ms ? 14 : 0) +
                            (ex.transcript[0] ? 26 : 0) +
                            26 * wrapped_line_count(ex.reply, kContentW - 16, 2, 2);
                    if (yy + h > kHistBottom) break;
                    yy += h + 6;
                    first = i;
                }
            }
            for (int i = first; i < v.history_count; ++i) {
                const HermesExchange& ex = v.history[i];
                const int r_lines =
                    wrapped_line_count(ex.reply, kContentW - 16, 2, 2);
                const int bh = 14 + (ex.end_ms ? 14 : 0) +
                               (ex.transcript[0] ? 26 : 0) + 26 * r_lines;
                aiwatchos::BubbleStyle hist = aiwatchos::bubble_style();
                hist.top = hc::kBarTop;
                hist.mid = hc::kBar;
                hist.bot = hc::kBarBot;
                hist.shadow = nullptr;
                fb.draw_bubble(8, y, aiwatchos::kDisplayWidth - 16, bh, 14,
                               hist);
                int iy = y + 7;
                if (ex.end_ms) {
                    const uint64_t disp =
                        aiwatchos::local_display_ms(ex.end_ms);
                    char stamp[8];
                    std::snprintf(stamp, sizeof(stamp), "%02u:%02u",
                                  static_cast<unsigned>((disp / 3600000) % 24),
                                  static_cast<unsigned>((disp / 60000) % 60));
                    aiwatchos::draw_text_aa(fb, 24, iy, stamp,
                                            aiwatchos::FontFace::kMicro,
                                            kAmberColor);
                    iy += 14;
                }
                if (ex.transcript[0]) {
                    char you[528];
                    std::snprintf(you, sizeof(you), "YOU: %s", ex.transcript);
                    iy = draw_wrapped(fb, 24, iy, kContentW - 16, you, 2,
                                      kDimColor, 1);
                }
                if (ex.reply[0]) {
                    draw_wrapped(fb, 24, iy, kContentW - 16, ex.reply, 2,
                                 kTextColor, 2);
                }
                y += bh + 6;
            }
        }
        if (v.transcript[0]) {
            y = draw_wrapped(fb, 16, y, kContentW, "YOU:", 2, kStatusColor, 1);
            y = draw_wrapped(fb, 16, y, kContentW, v.transcript, 2, kDimColor, 3);
            y += 6;
        }
        if (v.reply[0]) {
            draw_wrapped(fb, 16, y, kContentW, v.reply, 2, kTextColor, 12);
        } else if (v.link == HermesLinkState::Linked && !v.busy) {
            // One-tap smoke message + push-to-talk affordance.
            aiwatchos::BubbleStyle hello_style;
            hello_style.top = aiwatchos::blend565(kAmberColor, aiwatchos::colors::kWhite, 90);
            hello_style.mid = kAmberColor;
            hello_style.bot = aiwatchos::blend565(kAmberColor, aiwatchos::colors::kBlack, 60);
            hello_style.border_alpha = 100;
            hello_style.shadow = nullptr;
            fb.draw_bubble(105, 300, 200, 48, 16, hello_style);
            const int hello_w = text_w("SAY HELLO", 3);
            draw_text(fb, 105 + (200 - hello_w) / 2, 300 + 7, "SAY HELLO", 3,
                      kHermesBg);
            draw_text(fb, 16, 364, "OR HOLD BOTTOM TO TALK", 2, kStatusColor);
        } else if (v.link == HermesLinkState::Offline) {
            draw_wrapped(fb, 16, y, kContentW,
                         "NO LINK. THE WATCH JOINS WIFI AND CONNECTS TO THE HERMES HUB AUTOMATICALLY.",
                         2, kDimColor, 6);
            if (v.error[0]) {
                draw_wrapped(fb, 16, y + 92, kContentW, v.error, 2, aiwatchos::colors::kDanger, 2);
            }
        } else if (v.link == HermesLinkState::Connecting) {
            draw_text(fb, 16, y, "CONNECTING...", 2, kDimColor);
        } else if (v.link == HermesLinkState::Pairing) {
            draw_text(fb, 16, y, "WAITING FOR PAIRING CODE...", 2, kDimColor);
        }
        if (v.notice[0] && now_ms() < v.notice_until_ms) {
            draw_wrapped(fb, 16, 424, kContentW, v.notice, 2, kAmberColor, 1);
        }
    }

    // Hint floats over the background (no band — the shell owns the
    // only chrome at the top; the bottom stays clean).
    // H5: while BOOT is held past the hint threshold, count the hold
    // down over the bottom hint line.
    if (g_boot_hint_ms >= 600 && !g_boot_fired) {
        const uint32_t left_ms = 2000 - g_boot_hint_ms;
        char hold[48];
        std::snprintf(hold, sizeof(hold), "KEEP HOLDING: %s IN %u.%us",
                      v.link == HermesLinkState::AuthFailed
                          ? "FORGET + RE-ENROLL"
                          : "NEW SESSION",
                      static_cast<unsigned>(left_ms / 1000),
                      static_cast<unsigned>((left_ms % 1000) / 100));
        draw_text(fb, 16, aiwatchos::kDisplayHeight - 60, hold, 2, kAmberColor);
    }
    const char* hint = "HOLD BOTTOM: TALK";
    if (v.prompt_active) hint = "TAP YES OR NO";
    else if (v.card_active || v.image_active) hint = "TAP: DISMISS";
    else if (v.link != HermesLinkState::Linked) hint = "HERMES GADGET";
    draw_text(fb, 16, aiwatchos::kDisplayHeight - 30, hint, 2, kDimColor);
}

bool hermes_on_touch(const aiwatchos::TouchEvent& event) {
    if (!g_client) return false;
    LockGuard lock;
    const HermesView& v = g_client->view();
    uint64_t now = now_ms();

    if (event.type == aiwatchos::TouchEvent::Press) {
        // Prompt first (guard: ignore presses in the first 0.6 s so a press
        // meant for something else doesn't answer — protocol doc rule).
        if (v.prompt_active && now - v.prompt_since_ms >= 600 && event.y >= 340) {
            return g_client->answer_prompt(event.x < aiwatchos::kDisplayWidth / 2);
        }
        // Cards/images dismiss on tap.
        if (v.card_active || v.image_active) {
            g_client->dismiss_card();
            return true;
        }
        // Top edge: cancel capture / running turn.
        if (event.y < 60) {
            g_client->cancel(now);
            return true;
        }
        // SAY HELLO button (only drawn in the linked+idle state).
        if (v.link == HermesLinkState::Linked && !v.busy && !v.reply[0] &&
            event.x >= 105 && event.x <= 305 && event.y >= 300 && event.y <= 348) {
            return g_client->send_chat_text("Hello from my watch");
        }
        // Bottom-center hold: push-to-talk.
        if (event.y > aiwatchos::kDisplayHeight - 100) {
            return g_client->talk_start(now);
        }
        return false;
    }

    if (event.type == aiwatchos::TouchEvent::Release &&
        event.y > aiwatchos::kDisplayHeight - 100) {
        if (g_client->talk_active()) {
            g_client->talk_stop(now);
            return true;
        }
    }
    return false;
}

aiwatchos::App make_hermes_app(aiwatchos::Hal& hal) {
    g_hal = &hal;
    hermes_init();   // boot the client + link at OS start, not first launch
    return aiwatchos::make_app(
        "hermes",
        hermes_init,      // init
        hermes_tick,      // tick
        hermes_render,    // render
        hermes_on_touch   // on_touch
    );
}

// ---------------------------------------------------------------------------
// Host-test hooks (host build only — see header)
// ---------------------------------------------------------------------------

#ifndef __ESPRESSIF_IDF__
void hermes_test_restore() { hermes_restore_persisted(); }

void hermes_test_reset(const uint8_t key[32]) {
    ensure_client();
    if (!g_client) return;
    HermesSinks sinks{};
    sinks.send_text = &sink_send_text;
    sinks.send_binary = &sink_send_binary;
    sinks.audio_begin = &sink_audio_begin;
    sinks.audio_write = &sink_audio_write;
    sinks.audio_end = &sink_audio_end;
    sinks.audio_abort = &sink_audio_abort;
    sinks.ota_begin = &sink_ota_begin;
    sinks.ota_write = &sink_ota_write;
    sinks.ota_finish = &sink_ota_finish;
    sinks.ota_abort = &sink_ota_abort;
    sinks.fill_random = &sink_fill_random;
    sinks.alloc_big = &sink_alloc_big;
    sinks.free_big = &sink_free_big;
    HermesConfig cfg{};
    g_client->init(sinks, cfg);
    g_client->set_key(key);
    g_outbound.clear();
    hermes_link_test_ota_reset();
}

void hermes_test_transport_up() { hermes_core_transport_up(); }
void hermes_test_transport_down() { hermes_core_transport_down(); }
void hermes_test_inject_text(const char* json) {
    hermes_core_transport_text(json, std::strlen(json));
}
void hermes_test_inject_binary(const uint8_t* data, size_t len) {
    hermes_core_transport_binary(data, len);
}
void hermes_test_talk_feed(const int16_t* pcm, size_t frames) {
    hermes_core_talk_feed(pcm, frames);
}
size_t hermes_test_outbound_count() { return g_outbound.size(); }
bool hermes_test_outbound_is_binary(size_t i) {
    return i < g_outbound.size() && g_outbound[i].binary;
}
std::string hermes_test_outbound_text(size_t i) {
    if (i >= g_outbound.size() || g_outbound[i].binary) return std::string();
    return std::string(g_outbound[i].bytes.begin(), g_outbound[i].bytes.end());
}
void hermes_test_clear_outbound() { g_outbound.clear(); }
size_t hermes_test_ota_captured(const uint8_t** out) {
    if (out) *out = hermes_link_test_ota_data();
    return hermes_link_test_ota_size();
}
#endif

}  // namespace aiwatchos_hermes
