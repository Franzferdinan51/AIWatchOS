// Muse link: WSS + Noise XX transport to the Hatch VM, driving the Muse
// app's outbox. Ported from muse-gadget-206's muse_chat_session.cpp (the
// working reference firmware for this same board) and adapted:
//
//   - ESP-IDF 5.3.6 APIs (the gadget targets 5.5+/6.x): esp_tls sync
//     connect, esp_crt_bundle, classic event APIs — same design, 5.3
//     spellings.
//   - Scope trimmed to what AIWatchOS's Muse app actually does today:
//     push-to-talk voice-note turns out, text replies in. The gadget's
//     dictation stream, typed-turn path, and MP3 fetch machinery have no
//     counterpart here (see "Replies" note below).
//   - Capability honesty (the gadget's own link.register bug is the
//     cautionary tale: its handlers exist but are never advertised):
//     this client advertises nothing it doesn't implement. The chat
//     protocol's only capability claims are the request headers and the
//     note body's "output_modality":"text" — and text is exactly what it
//     consumes back.
//
// Replies: the hatch chat protocol returns assistant TEXT on the
// /chat/subscribe event stream (delta.* / message.* events). There is no
// audio reply stream in the protocol — the reference gadget synthesizes
// speech locally from that text ("a TTS API of your choice" is its own
// comment). So replies land in muse_link_last_reply() for the UI, and the
// Muse app's MP3 playback path (muse_play_reply) stays ready for any
// future spoken-reply source; nothing here fakes an MP3 stream.
//
// Turn model (matches the reference): a completed PTT turn is one
// POST /chat/stream whose body is a JSON voice note — base64 WAV of the
// turn's 16 kHz PCM (decoded here from the outbox's IMA-ADPCM). The ack
// names the user message ids; reply events on the subscribe stream are
// accepted only when they answer one of those ids.
#include "aiwatchos_net/net.hpp"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <xplat/noise/core/ClientSession.h>
#include <xplat/noise/core/PsaCryptoBackend.h>

#include "aiwatchos_net/net_proto.hpp"
#include "muse_account.hpp"

namespace aiwatchos_net {
namespace {

using namespace musegadgets::noise::core;
using musegadgets::noise::core::Span;

const char* kTag = "aiw-muse-link";

constexpr size_t kScratch = 64 * 1024;
constexpr int64_t kIoTimeoutUs = 10 * 1000 * 1000;
constexpr int64_t kPingIntervalUs = 20 * 1000 * 1000;
constexpr int64_t kDeadUs = 60 * 1000 * 1000;
constexpr int64_t kAckTimeoutUs = 30 * 1000 * 1000;
constexpr int64_t kFirstReplyTimeoutUs = 60 * 1000 * 1000;
constexpr int64_t kTurnTimeoutUs = 180 * 1000 * 1000;
constexpr int64_t kSettleUs = 3 * 1000 * 1000;
constexpr size_t kMaxReplyChars = 4096;
constexpr size_t kMaxSubLine = 128 * 1024;

int64_t now_us() { return esp_timer_get_time(); }

void* psram_alloc(size_t n) {
    // Budget-audit rule (docs/ui/RESOURCE_BUDGET.md, red flag 2): large
    // buffers are PSRAM-strict. heap_caps_malloc_prefer would silently
    // fall back to internal DRAM if PSRAM allocation failed — a single
    // 64 KB scratch buffer landing in DRAM would detonate the heap.
    // Allocations >16 KB therefore fail closed (nullptr) instead.
    if (n > 16 * 1024) {
        return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_DEFAULT);
}

// --- TLS write/read helpers (from the gadget's hatch-link port) -----------

bool write_all(esp_tls_t* tls, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    int64_t deadline = now_us() + kIoTimeoutUs;
    while (len) {
        if (now_us() >= deadline) return false;
        ssize_t n = esp_tls_conn_write(tls, p, len);
        if (n > 0) {
            p += n;
            len -= static_cast<size_t>(n);
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ ||
                   n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            return false;
        }
    }
    return true;
}

bool read_all(esp_tls_t* tls, uint8_t* buf, size_t len) {
    int64_t deadline = now_us() + kIoTimeoutUs;
    while (len) {
        if (now_us() >= deadline) return false;
        ssize_t n = esp_tls_conn_read(tls, buf, len);
        if (n > 0) {
            buf += n;
            len -= static_cast<size_t>(n);
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ ||
                   n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            return false;
        }
    }
    return true;
}

bool ws_send_binary(esp_tls_t* tls, const uint8_t* payload, size_t len) {
    uint8_t hdr[14];
    size_t hl;
    hdr[0] = 0x82;  // FIN | binary
    if (len < 126) {
        hdr[1] = static_cast<uint8_t>(0x80 | len);
        hl = 2;
    } else if (len <= 65535) {
        hdr[1] = 0x80 | 126;
        hdr[2] = static_cast<uint8_t>(len >> 8);
        hdr[3] = static_cast<uint8_t>(len & 0xff);
        hl = 4;
    } else {
        hdr[1] = 0x80 | 127;
        for (int i = 0; i < 8; ++i) hdr[2 + i] = static_cast<uint8_t>(len >> (56 - 8 * i));
        hl = 10;
    }
    uint32_t key = esp_random();
    std::memcpy(hdr + hl, &key, 4);
    hl += 4;
    if (!write_all(tls, hdr, hl)) return false;
    const uint8_t* mk = reinterpret_cast<const uint8_t*>(&key);
    uint8_t chunk[512];
    for (size_t off = 0; off < len;) {
        size_t n = len - off < sizeof(chunk) ? len - off : sizeof(chunk);
        for (size_t i = 0; i < n; ++i) chunk[i] = payload[off + i] ^ mk[(off + i) & 3];
        if (!write_all(tls, chunk, n)) return false;
        off += n;
    }
    return true;
}

bool ws_send_ping(esp_tls_t* tls) {
    uint8_t frame[6] = {0x89, 0x80};
    uint32_t key = esp_random();
    std::memcpy(frame + 2, &key, 4);
    return write_all(tls, frame, sizeof(frame));
}

// One WS frame. wait=false: -2 at once when nothing is pending. Returns
// payload length, -3 for ping/pong (handled), 0 peer-closed, -1 error.
ssize_t ws_recv(esp_tls_t* tls, uint8_t* buf, size_t cap, bool wait) {
    uint8_t hdr[2];
    int64_t deadline = now_us() + kIoTimeoutUs;
    for (;;) {
        ssize_t n = esp_tls_conn_read(tls, hdr, 1);
        if (n == 1) break;
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            if (!wait) return -2;
            if (now_us() >= deadline) return -1;
            vTaskDelay(1);
            continue;
        }
        return n == 0 ? 0 : -1;
    }
    if (!read_all(tls, hdr + 1, 1)) return -1;
    uint8_t opcode = hdr[0] & 0x0f;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t plen = hdr[1] & 0x7f;
    uint8_t ext[8];
    if (plen == 126) {
        if (!read_all(tls, ext, 2)) return -1;
        plen = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (plen == 127) {
        if (!read_all(tls, ext, 8)) return -1;
        plen = 0;
        for (int i = 0; i < 8; ++i) plen = (plen << 8) | ext[i];
    }
    uint8_t mask[4] = {0};
    if (masked && !read_all(tls, mask, 4)) return -1;
    if (plen > cap) {
        ESP_LOGE(kTag, "ws frame too large: %llu",
                 static_cast<unsigned long long>(plen));
        return -1;
    }
    if (!read_all(tls, buf, static_cast<size_t>(plen))) return -1;
    if (masked) {
        for (size_t i = 0; i < plen; ++i) buf[i] ^= mask[i & 3];
    }
    if (opcode == 0x8) return -1;  // close
    if (opcode == 0x9) {
        uint8_t pong[6] = {0x8A, static_cast<uint8_t>(0x80 | plen), 0, 0, 0, 0};
        write_all(tls, pong, sizeof(pong));
        if (plen) write_all(tls, buf, static_cast<size_t>(plen));
        return -3;
    }
    if (opcode == 0xA) return -3;
    return static_cast<ssize_t>(plen);
}

// --- Connection state ------------------------------------------------------

struct Conn {
    esp_tls_t* tls = nullptr;
    ClientSession* session = nullptr;
    PsaCryptoBackend* crypto = nullptr;
    uint8_t* ws = nullptr;   // outbound payload scratch (>= kMaxOutbound...)
    uint8_t* rx = nullptr;
    uint8_t* tf = nullptr;
    uint8_t* sr = nullptr;
    uint8_t* svc = nullptr;
    uint8_t* env = nullptr;
    int64_t next_id = 1;
    int64_t last_rx_us = 0;
    int64_t last_ping_us = 0;
    bool up = false;
};

Conn g_conn;
std::atomic<LinkState> g_state{LinkState::kOff};

enum StreamKind { kStreamNone = 0, kStreamSub = 1, kStreamChat = 2 };
struct StreamSlot {
    int64_t id = 0;
    int kind = kStreamNone;
    int status = 0;
};
StreamSlot g_sub, g_chat;
std::string g_sub_line;    // NDJSON line assembly for the subscribe stream
std::string g_chat_body;   // ack body assembly for the chat stream

void teardown() {
    if (g_conn.tls) esp_tls_conn_destroy(g_conn.tls);
    if (g_conn.session) {
        g_conn.session->~ClientSession();
        heap_caps_free(g_conn.session);
    }
    if (g_conn.crypto) {
        g_conn.crypto->~PsaCryptoBackend();
        heap_caps_free(g_conn.crypto);
    }
    uint8_t* bufs[] = {g_conn.ws, g_conn.rx, g_conn.tf,
                       g_conn.sr, g_conn.svc, g_conn.env};
    for (uint8_t* b : bufs) heap_caps_free(b);
    g_conn = Conn{};
    g_sub = StreamSlot{};
    g_chat = StreamSlot{};
    g_sub_line.clear();
    g_chat_body.clear();
}

bool flush_outbound() {
    while (g_conn.session->HasOutboundWebSocketPayload()) {
        auto r = g_conn.session->WriteNextOutboundWebSocketPayload(
            ByteSpan(g_conn.ws, ClientSession::kMaxOutboundWebSocketPayloadSize));
        if (!r.ok() || !ws_send_binary(g_conn.tls, g_conn.ws, r.size())) {
            ESP_LOGE(kTag, "outbound flush failed");
            return false;
        }
    }
    return true;
}

// Opens a Daemon-service request stream (same shape as the gadget's
// open_stream: x-request-id + x-app-id headers, optional Content-Type /
// accept). Returns the stream id, or 0 on failure.
int64_t open_stream(const char* verb, const char* path,
                    const char* content_type, const char* accept,
                    const char* body, bool end_body, StreamSlot* slot) {
    char req_id[40];
    std::snprintf(req_id, sizeof(req_id), "muse-%08lx-%08lx",
                  static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(esp_random()));
    HeaderView hdrs[4];
    size_t nh = 0;
    hdrs[nh++] = {StringView("x-request-id"), StringView(req_id)};
    hdrs[nh++] = {StringView("x-app-id"), StringView("hatch-web")};
    if (content_type) hdrs[nh++] = {StringView("Content-Type"), StringView(content_type)};
    if (accept) hdrs[nh++] = {StringView("accept"), StringView(accept)};

    int64_t id = g_conn.next_id++;  // reference firmware: sequential from 1
    ApplicationRequestView req;
    req.verb = StringView(verb);
    req.path = StringView(path);
    req.headers = Span<const HeaderView>(hdrs, nh);
    req.body = body ? ConstByteSpan(reinterpret_cast<const uint8_t*>(body),
                                    std::strlen(body))
                    : ConstByteSpan();
    req.end_body = end_body;
    auto r = g_conn.session->StartOutboundApplicationRequest(
        ServiceType::Daemon, id, req, ByteSpan(g_conn.svc, kScratch),
        ByteSpan(g_conn.env, kScratch));
    if (!r.ok()) {
        ESP_LOGE(kTag, "%s %s: %s", verb, path, r.status().str());
        return 0;
    }
    if (!flush_outbound()) return 0;
    slot->id = id;
    slot->status = 0;
    ESP_LOGI(kTag, "stream %lld: %s %s", static_cast<long long>(id), verb, path);
    return id;
}

bool send_body(int64_t id, const uint8_t* data, size_t len, bool end_body) {
    BodyChunkView chunk{ConstByteSpan(data, len), end_body};
    auto r = g_conn.session->StartOutboundBodyChunk(
        ServiceType::Daemon, id, chunk, ByteSpan(g_conn.svc, kScratch),
        ByteSpan(g_conn.env, kScratch));
    if (!r.ok()) {
        ESP_LOGE(kTag, "body chunk: %s", r.status().str());
        return false;
    }
    return flush_outbound();
}

bool send_reset(int64_t id) {
    ResetView rv{ResetCode::Cancelled, StringView("cancelled")};
    auto r = g_conn.session->StartOutboundReset(
        ServiceType::Daemon, id, rv, ByteSpan(g_conn.svc, kScratch),
        ByteSpan(g_conn.env, kScratch));
    return r.ok() && flush_outbound();
}

// --- Turn receive state ----------------------------------------------------
// One upload at a time (the link task is the only caller), so plain statics.

struct RxMsg {
    std::string id;
    std::string text;
    bool done = false;
};

struct TurnRx {
    bool acked = false;
    std::string user_ids[2];
    int64_t last_seq = -1;
    std::vector<RxMsg> msgs;
    int64_t last_event_us = 0;
    bool any_event = false;
};
TurnRx g_rx;

SemaphoreHandle_t g_reply_mutex = nullptr;
std::string g_last_reply;

void turn_rx_reset() {
    g_rx = TurnRx{};
}

bool id_in_user_ids(const std::string& id) {
    if (id.empty()) return false;
    return id == g_rx.user_ids[0] || id == g_rx.user_ids[1];
}

// Before the ack lands we don't know our user ids yet: everything relates.
// After it, only events answering one of those ids count.
bool event_relates(const std::string& reply_to) {
    if (!g_rx.acked) return true;
    if (reply_to.empty()) return true;
    return id_in_user_ids(reply_to);
}

RxMsg* find_msg(const std::string& id, bool create) {
    for (auto& m : g_rx.msgs) {
        if (m.id == id) return &m;
    }
    if (!create || id.empty() || g_rx.msgs.size() >= 4) return nullptr;
    g_rx.msgs.push_back(RxMsg{id, "", false});
    return &g_rx.msgs.back();
}

std::string json_str_field(const cJSON* o, const char* a, const char* b = nullptr) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, a);
    if (!v && b) v = cJSON_GetObjectItemCaseSensitive(o, b);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

// One NDJSON line from the subscribe stream (see muse_chat_events in the
// reference): {type:"event", seq, event, payload}.
void on_sub_line(const char* line) {
    cJSON* root = cJSON_Parse(line);
    if (!root) return;
    std::string type = json_str_field(root, "type");
    if (type == "event") {
        const cJSON* seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
        if (cJSON_IsNumber(seq)) {
            int64_t s = static_cast<int64_t>(seq->valuedouble);
            if (s <= g_rx.last_seq) { cJSON_Delete(root); return; }  // dup
            g_rx.last_seq = s;
        }
        std::string ev = json_str_field(root, "event");
        const cJSON* p = cJSON_GetObjectItemCaseSensitive(root, "payload");
        if (p) {
            std::string mid = json_str_field(p, "message_id", "id");
            std::string reply_to =
                json_str_field(p, "reply_to_message_id", "parent_message_id");
            bool known = ev.rfind("delta.", 0) == 0 || ev == "message.assistant";
            if (known && event_relates(reply_to)) {
                if (ev == "delta.message_start") {
                    find_msg(mid, true);
                } else if (ev == "delta.text_append") {
                    if (RxMsg* m = find_msg(mid, true)) {
                        std::string t = json_str_field(p, "text");
                        if (m->text.size() + t.size() <= kMaxReplyChars)
                            m->text += t;
                    }
                } else if (ev == "delta.message_done" ||
                           ev == "message.assistant") {
                    if (RxMsg* m = find_msg(mid, true)) {
                        std::string full =
                            json_str_field(p, "display_text", "content");
                        if (full.empty()) full = json_str_field(p, "text");
                        if (!full.empty()) m->text = full;
                        m->done = true;
                    }
                }
                g_rx.any_event = true;
                g_rx.last_event_us = now_us();
            } else if (ev == "agent.status" || ev == "task.status") {
                g_rx.any_event = true;  // activity counts for settle timing
                g_rx.last_event_us = now_us();
            }
        }
    }
    cJSON_Delete(root);
}

// The chat stream's whole body is one JSON ack:
// {result:{message_id, reply_to_message_id}} names our user messages.
void on_chat_body(const std::string& body) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return;
    const cJSON* res = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!res) res = root;
    std::string a = json_str_field(res, "message_id");
    std::string b = json_str_field(res, "reply_to_message_id");
    int k = 0;
    if (!a.empty()) g_rx.user_ids[k++] = a;
    if (!b.empty() && b != a) g_rx.user_ids[k++] = b;
    if (k > 0) {
        g_rx.acked = true;
        ESP_LOGI(kTag, "chat ack: user message %s", g_rx.user_ids[0].c_str());
    }
    cJSON_Delete(root);
}

void stream_data(StreamSlot& s, ConstByteSpan data) {
    if (data.empty()) return;
    if (s.kind == kStreamSub) {
        g_sub_line.append(reinterpret_cast<const char*>(data.data()), data.size());
        if (g_sub_line.size() > kMaxSubLine) {  // runaway line: drop it
            g_sub_line.clear();
            return;
        }
        size_t nl;
        while ((nl = g_sub_line.find('\n')) != std::string::npos) {
            std::string line = g_sub_line.substr(0, nl);
            g_sub_line.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) on_sub_line(line.c_str());
        }
    } else if (s.kind == kStreamChat) {
        if (g_chat_body.size() + data.size() <= 64 * 1024)
            g_chat_body.append(reinterpret_cast<const char*>(data.data()), data.size());
    }
}

void stream_end(StreamSlot& s, bool ok) {
    if (s.kind == kStreamChat && ok) on_chat_body(g_chat_body);
    if (s.kind == kStreamChat) g_chat_body.clear();
    s.kind = kStreamNone;
    s.id = 0;
}

bool on_frame(const DecodedServiceFrame& f) {
    StreamSlot* s = nullptr;
    if (g_sub.kind != kStreamNone && f.stream_id == g_sub.id) s = &g_sub;
    else if (g_chat.kind != kStreamNone && f.stream_id == g_chat.id) s = &g_chat;
    if (!s) return true;
    switch (f.kind) {
        case ServiceFrameKind::Response:
            s->status = f.response.status;
            if (f.response.status >= 400) {
                ESP_LOGW(kTag, "stream %lld http %ld",
                         static_cast<long long>(f.stream_id),
                         static_cast<long>(f.response.status));
                stream_end(*s, false);
                return true;
            }
            stream_data(*s, f.response.body);
            if (f.response.end_body) stream_end(*s, true);
            break;
        case ServiceFrameKind::BodyChunk:
            stream_data(*s, f.body_chunk.data);
            if (f.body_chunk.end_body) stream_end(*s, true);
            break;
        case ServiceFrameKind::Reset:
            ESP_LOGW(kTag, "stream %lld reset", static_cast<long long>(f.stream_id));
            stream_end(*s, false);
            break;
        default:
            break;
    }
    return true;
}

// Drains pending inbound frames. False = connection failed.
bool poll_socket() {
    static HeaderView hdrs[16];
    for (int budget = 0; budget < 8; ++budget) {
        ssize_t n = ws_recv(g_conn.tls, g_conn.rx, kScratch, false);
        if (n == -2) return true;
        if (n == 0 || n == -1) {
            ESP_LOGW(kTag, "%s", n ? "receive error" : "server closed");
            return false;
        }
        g_conn.last_rx_us = now_us();
        if (n == -3) continue;
        auto in = g_conn.session->ProcessInboundWebSocketPayload(
            ConstByteSpan(g_conn.rx, static_cast<size_t>(n)),
            ByteSpan(g_conn.tf, kScratch), ByteSpan(g_conn.sr, kScratch),
            Span<HeaderView>(hdrs, 16));
        if (!in.ok()) {
            ESP_LOGW(kTag, "inbound frame: %s", in.status.str());
            return false;
        }
        if (in.frame_status == InboundFrameStatus::Complete && !on_frame(in.frame))
            return false;
        if (!g_conn.up) return false;
    }
    return true;
}

// --- VM resolution ---------------------------------------------------------
// Cached after the first success; cleared whenever a connect fails so a
// rotated VM token is picked up on the next attempt.

std::string g_vm_id, g_vm_token, g_host;
bool g_resolved = false;

bool resolve_vm(const NetConfig& cfg) {
    if (g_resolved) return true;
    std::string vm_id, token, host;
    if (!cfg.vm_token.empty()) {
        // Direct mode: a per-VM token was configured; no account API.
        vm_id = cfg.vm_id;
        token = cfg.vm_token;
        host = proto::derive_host(cfg.host, "", vm_id, cfg.domain);
    } else {
        VmInfo info = account_find_vm(cfg.hatch_token, cfg.vm_id);
        if (info.ok) {
            vm_id = info.vm_id.empty() ? cfg.vm_id : info.vm_id;
            token = info.token;
            host = proto::derive_host(cfg.host, info.ws_url, vm_id, cfg.domain);
        } else if (!cfg.vm_id.empty()) {
            // Gadget behavior: if the account lookup fails but a VM was
            // named, try the configured token as that VM's own token.
            ESP_LOGW(kTag, "account lookup failed; trying token directly for vm %s",
                     cfg.vm_id.c_str());
            vm_id = cfg.vm_id;
            token = cfg.hatch_token;
            host = proto::derive_host(cfg.host, "", vm_id, cfg.domain);
        }
    }
    if (vm_id.empty() || token.empty() || host.empty()) {
        ESP_LOGE(kTag, "cannot resolve vm (id '%s', host '%s')", vm_id.c_str(),
                 host.c_str());
        return false;
    }
    g_vm_id = vm_id;
    g_vm_token = token;
    g_host = host;
    g_resolved = true;
    ESP_LOGI(kTag, "vm %s @ %s", vm_id.c_str(), host.c_str());
    return true;
}

// --- Connect: TLS -> WS upgrade -> Noise XX -> subscribe -------------------

int ws_upgrade(esp_tls_t* tls, const std::string& vm_id,
               const std::string& token, const std::string& host) {
    std::string esc = proto::query_escape(vm_id);
    std::string req = "GET /v1/noise?vm_id=" + esc +
                      " HTTP/1.1\r\nHost: " + host +
                      "\r\nAuthorization: Bearer " + token +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Version: 13\r\n"
                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    if (!write_all(tls, req.data(), req.size())) return 0;
    // Edge error responses carry a ~1.4 KB Proxy-Status header; read to 4 KB.
    char hdr[4096];
    size_t have = 0;
    int64_t deadline = now_us() + kIoTimeoutUs;
    while (now_us() < deadline && have < sizeof(hdr)) {
        ssize_t n = esp_tls_conn_read(tls, hdr + have, sizeof(hdr) - have);
        if (n > 0) {
            size_t from = have >= 3 ? have - 3 : 0;
            have += static_cast<size_t>(n);
            bool end = false;
            for (size_t i = from; i + 3 < have; ++i) {
                if (!std::memcmp(hdr + i, "\r\n\r\n", 4)) { end = true; break; }
            }
            if (end) break;
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ ||
                   n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            break;
        }
    }
    if (have >= 12 && !std::memcmp(hdr, "HTTP/", 5)) return std::atoi(hdr + 9);
    return 0;
}

bool connect_once(const NetConfig& cfg) {
    if (!resolve_vm(cfg)) return false;

    g_conn.ws = static_cast<uint8_t*>(
        psram_alloc(ClientSession::kMaxOutboundWebSocketPayloadSize));
    g_conn.rx = static_cast<uint8_t*>(psram_alloc(kScratch));
    g_conn.tf = static_cast<uint8_t*>(psram_alloc(kScratch));
    g_conn.sr = static_cast<uint8_t*>(psram_alloc(kScratch));
    g_conn.svc = static_cast<uint8_t*>(psram_alloc(kScratch));
    g_conn.env = static_cast<uint8_t*>(psram_alloc(kScratch));
    void* crypto_mem = psram_alloc(sizeof(PsaCryptoBackend));
    void* session_mem = psram_alloc(sizeof(ClientSession));
    if (!g_conn.ws || !g_conn.rx || !g_conn.tf || !g_conn.sr || !g_conn.svc ||
        !g_conn.env || !crypto_mem || !session_mem) {
        ESP_LOGE(kTag, "scratch alloc failed");
        return false;
    }
    g_conn.crypto = new (crypto_mem) PsaCryptoBackend();
    g_conn.session = new (session_mem) ClientSession(*g_conn.crypto);
    g_conn.next_id = 1;

    esp_tls_cfg_t tcfg{};
    tcfg.crt_bundle_attach = esp_crt_bundle_attach;
    tcfg.timeout_ms = 15000;
    g_conn.tls = esp_tls_init();
    if (!g_conn.tls) return false;
    int r = esp_tls_conn_new_sync(g_host.c_str(),
                                  static_cast<int>(g_host.size()), 443, &tcfg,
                                  g_conn.tls);
    if (r != 1) {
        ESP_LOGE(kTag, "tls connect to %s failed", g_host.c_str());
        return false;
    }
    int status = ws_upgrade(g_conn.tls, g_vm_id, g_vm_token, g_host);
    if (status != 101) {
        ESP_LOGE(kTag, "ws upgrade status %d", status);
        return false;
    }
    // Noise XX handshake; msg3's payload is empty — the bearer header on
    // the upgrade already authenticated us (reference behavior).
    ByteSpan out(g_conn.ws, ClientSession::kMaxOutboundWebSocketPayloadSize);
    auto m1 = g_conn.session->WriteHandshakeMessage1(out);
    if (!m1.ok() || !ws_send_binary(g_conn.tls, g_conn.ws, m1.size())) {
        ESP_LOGE(kTag, "handshake msg1 failed");
        return false;
    }
    ssize_t n = ws_recv(g_conn.tls, g_conn.rx, kScratch, true);
    if (n <= 0) {
        ESP_LOGE(kTag, "handshake msg2 not received");
        return false;
    }
    uint8_t extra[256];
    size_t extra_len = 0;
    Status st = g_conn.session->ReadHandshakeMessage2(
        ConstByteSpan(g_conn.rx, static_cast<size_t>(n)),
        ByteSpan(extra, sizeof(extra)), extra_len);
    if (!st.ok()) {
        ESP_LOGE(kTag, "handshake msg2: %s", st.str());
        return false;
    }
    auto m3 = g_conn.session->WriteHandshakeMessage3(ConstByteSpan(), out);
    if (!m3.ok() || !ws_send_binary(g_conn.tls, g_conn.ws, m3.size())) {
        ESP_LOGE(kTag, "handshake msg3 failed");
        return false;
    }
    if (!g_conn.session->isEstablished()) {
        ESP_LOGE(kTag, "session not established after handshake");
        return false;
    }
    g_sub.kind = kStreamSub;
    if (!open_stream("POST", "/chat/subscribe", "application/json",
                     "application/x-ndjson", "{}", true, &g_sub)) {
        return false;
    }
    g_conn.up = true;
    g_conn.last_rx_us = g_conn.last_ping_us = now_us();
    turn_rx_reset();
    ESP_LOGI(kTag, "linked to %s (vm %s)", g_host.c_str(), g_vm_id.c_str());
    return true;
}

// --- IMA-ADPCM decode (mirrors apps/muse/src/muse_adpcm.c exactly; a local
// copy keeps the component graph acyclic — apps/muse depends on this
// component for the pump registration, not the other way round) -----------

const int16_t kAdpcmSteps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
    209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
    796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767,
};
const int8_t kAdpcmIndexStep[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

struct AdpcmDecoder {
    int32_t pred = 0;
    int32_t index = 0;
    int16_t decode(uint8_t code) {
        int st = kAdpcmSteps[index];
        int diff = st >> 3;
        if (code & 4) diff += st;
        if (code & 2) diff += st >> 1;
        if (code & 1) diff += st >> 2;
        int p = pred + (code & 8 ? -diff : diff);
        pred = p > 32767 ? 32767 : (p < -32768 ? -32768 : p);
        int idx = index + kAdpcmIndexStep[code & 7];
        index = idx < 0 ? 0 : (idx > 88 ? 88 : idx);
        return static_cast<int16_t>(pred);
    }
};

// --- Turn upload -------------------------------------------------------------

OutboxPumpFn g_pump = nullptr;
void* g_pump_ctx = nullptr;

bool upload_turn_impl(const uint8_t* adpcm, size_t len) {
    turn_rx_reset();
    g_chat_body.clear();
    g_chat.kind = kStreamChat;
    int64_t chat = open_stream("POST", "/chat/stream", "application/json",
                               nullptr, nullptr, false, &g_chat);
    if (!chat) return false;
    auto fail = [&]() {
        send_reset(chat);
        g_chat.kind = kStreamNone;
        return false;
    };
    if (!send_body(chat, reinterpret_cast<const uint8_t*>(proto::kNoteHead),
                   std::strlen(proto::kNoteHead), false))
        return fail();

    // Stream the WAV out in note parts: 6144 stream bytes per part (the
    // 44-byte header counts toward the first part), each base64-encoding
    // to exactly one 8192-char body chunk.
    char* b64 = static_cast<char*>(psram_alloc(proto::kBodyChunkBytes + 1));
    uint8_t* stage = static_cast<uint8_t*>(psram_alloc(proto::kNotePartPcmBytes));
    if (!b64 || !stage) {
        heap_caps_free(b64);
        heap_caps_free(stage);
        return fail();
    }
    proto::wav_header(stage, 16000);
    size_t stage_len = proto::kWavHeaderBytes;
    AdpcmDecoder dec;
    bool ok = true;
    auto flush_part = [&]() {
        size_t n = proto::base64_encode(stage, stage_len, b64);
        bool sent = send_body(chat, reinterpret_cast<const uint8_t*>(b64), n, false);
        stage_len = 0;
        return sent;
    };
    for (size_t i = 0; i < len && ok; ++i) {
        uint8_t byte = adpcm[i];
        for (int half = 0; half < 2; ++half) {
            uint8_t code = half == 0 ? (byte & 0x0F) : (byte >> 4);
            int16_t s = dec.decode(code);
            stage[stage_len++] = static_cast<uint8_t>(s & 0xFF);
            stage[stage_len++] = static_cast<uint8_t>((s >> 8) & 0xFF);
            if (stage_len == proto::kNotePartPcmBytes) ok = flush_part();
        }
    }
    if (ok && stage_len > 0) ok = flush_part();
    if (ok) {
        ok = send_body(chat, reinterpret_cast<const uint8_t*>(proto::kNoteTail),
                       std::strlen(proto::kNoteTail), true);
    }
    heap_caps_free(b64);
    heap_caps_free(stage);
    if (!ok) return fail();
    ESP_LOGI(kTag, "voice note sent (%u adpcm bytes); waiting for reply",
             static_cast<unsigned>(len));

    // Collect the ack + reply events. The note is fully accepted at this
    // point: whatever happens below, the turn is NOT requeued.
    int64_t t0 = now_us();
    for (;;) {
        if (!poll_socket()) {
            g_conn.up = false;
            break;
        }
        int64_t now = now_us();
        if (!g_rx.acked) {
            if (now - t0 > kAckTimeoutUs) {
                ESP_LOGW(kTag, "no ack within timeout");
                break;
            }
        } else {
            bool all_done = !g_rx.msgs.empty();
            for (const auto& m : g_rx.msgs) all_done = all_done && m.done;
            if (all_done && now - g_rx.last_event_us > kSettleUs) break;
            if (!g_rx.any_event && now - t0 > kFirstReplyTimeoutUs) break;
            if (now - t0 > kTurnTimeoutUs) break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // Publish the reply text snapshot.
    std::string joined;
    for (const auto& m : g_rx.msgs) {
        if (m.text.empty()) continue;
        if (!joined.empty()) joined += "\n";
        joined += m.text;
    }
    if (!joined.empty() && g_reply_mutex) {
        xSemaphoreTake(g_reply_mutex, portMAX_DELAY);
        g_last_reply = joined;
        xSemaphoreGive(g_reply_mutex);
        ESP_LOGI(kTag, "reply: %u chars", static_cast<unsigned>(joined.size()));
    }
    return true;
}

// --- Link task ---------------------------------------------------------------

void link_task(void*) {
    int backoff_s = 5;
    int64_t last_pump_us = 0;
    for (;;) {
        const NetConfig& cfg = net_config();
        if (!cfg.muse_configured()) {
            g_state.store(LinkState::kOff);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (!wifi_connected()) {
            if (g_conn.up) teardown();
            g_state.store(LinkState::kWaitingWifi);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!g_conn.up) {
            g_state.store(LinkState::kConnecting);
            if (connect_once(cfg)) {
                g_state.store(LinkState::kLinked);
                backoff_s = 5;
            } else {
                teardown();
                g_resolved = false;
                g_state.store(LinkState::kBackoff);
                vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
                backoff_s = backoff_s * 2 > 120 ? 120 : backoff_s * 2;
            }
            continue;
        }
        if (!poll_socket()) {
            teardown();
            g_resolved = false;
            g_state.store(LinkState::kBackoff);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        int64_t now = now_us();
        if (now - g_conn.last_rx_us > kDeadUs) {
            ESP_LOGW(kTag, "link dead (no rx for 60s); reconnecting");
            teardown();
            g_resolved = false;
            continue;
        }
        if (now - g_conn.last_rx_us > kPingIntervalUs &&
            now - g_conn.last_ping_us > kPingIntervalUs) {
            if (!ws_send_ping(g_conn.tls)) {
                teardown();
                g_resolved = false;
                continue;
            }
            g_conn.last_ping_us = now;
        }
        // Drain queued turns through the registered pump (the Muse app's
        // outbox). The pump no-ops cheaply when the outbox is empty.
        if (g_pump && now - last_pump_us > 250 * 1000) {
            last_pump_us = now;
            g_pump(g_pump_ctx);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

}  // namespace

// --- Public API --------------------------------------------------------------

LinkState muse_link_state() { return g_state.load(); }

const char* muse_link_state_name(LinkState s) {
    switch (s) {
        case LinkState::kOff: return "OFFLINE";
        case LinkState::kWaitingWifi: return "NO WIFI";
        case LinkState::kConnecting: return "CONNECTING";
        case LinkState::kLinked: return "LINKED";
        case LinkState::kBackoff: return "RETRYING";
    }
    return "?";
}

void muse_link_set_outbox_pump(OutboxPumpFn fn, void* ctx) {
    g_pump = fn;
    g_pump_ctx = ctx;
}

bool muse_link_upload_turn(const uint8_t* adpcm, size_t len) {
    if (!g_conn.up || !adpcm) return false;
    return upload_turn_impl(adpcm, len);
}

bool muse_link_last_reply(char* out, size_t cap) {
    if (!out || cap == 0 || !g_reply_mutex) return false;
    xSemaphoreTake(g_reply_mutex, portMAX_DELAY);
    bool have = !g_last_reply.empty();
    std::strncpy(out, g_last_reply.c_str(), cap - 1);
    out[cap - 1] = 0;
    xSemaphoreGive(g_reply_mutex);
    return have;
}

void muse_link_task_start() {
    if (!g_reply_mutex) g_reply_mutex = xSemaphoreCreateMutex();
    xTaskCreate(link_task, "aiw_muse_link", 24576, nullptr, 5, nullptr);
}

}  // namespace aiwatchos_net
