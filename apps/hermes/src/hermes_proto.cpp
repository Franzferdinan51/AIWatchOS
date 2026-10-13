// Hermes Gadget Protocol v1 client state machine (see hermes_proto.hpp).
//
// Frames are parsed field-wise with cJSON and the document is freed before
// this function returns — nothing holds a parsed JSON tree across calls.
// Inbound strings are copied into the view's fixed buffers with caps, so a
// hostile or buggy server cannot grow device memory.
#include "hermes_proto.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"

namespace aiwatchos_hermes {
namespace {

void copy_capped(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) src = "";
    size_t n = std::strlen(src);
    if (n >= cap) n = cap - 1;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

const char* json_str(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (v && cJSON_IsString(v)) ? v->valuestring : nullptr;
}

bool json_int(const cJSON* obj, const char* key, long* out) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && cJSON_IsNumber(v)) {
        *out = static_cast<long>(v->valuedouble);
        return true;
    }
    return false;
}

bool json_bool(const cJSON* obj, const char* key, bool* out) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v && cJSON_IsBool(v)) {
        *out = cJSON_IsTrue(v);
        return true;
    }
    return false;
}

// Is `hex` exactly 64 hex chars? (OTA sha256 field sanity.)
bool is_sha256_hex(const char* hex) {
    if (!hex || std::strlen(hex) != 64) return false;
    for (int i = 0; i < 64; ++i) {
        char c = hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

char lower_hex(char c) {
    return (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

void HermesClient::init(const HermesSinks& sinks, const HermesConfig& cfg) {
    sinks_ = sinks;
    cfg_ = cfg;
    view_ = HermesView{};
    transport_up_ = false;
    welcomed_ = false;
    wants_reconnect_ = false;
    battery_pct_ = -1;
    battery_sent_ = -2;
    has_pending_text_ = false;
    capturing_ = false;
    play_stream_ = -1;
    img_stream_ = -1;
    ota_state_ = OtaState::Idle;
    ota_reboot_pending_ = false;
    if (img_buf_) {
        if (sinks_.free_big) sinks_.free_big(sinks_.ctx, img_buf_);
        else std::free(img_buf_);
        img_buf_ = nullptr;
    }
}

void HermesClient::set_key(const uint8_t key[32]) {
    std::memcpy(key_, key, 32);
    has_key_ = true;
    std::string id = hermes_device_id_for_key(key_);
    copy_capped(device_id_, sizeof(device_id_), id.c_str());
}

void HermesClient::set_link(HermesLinkState s) { view_.link = s; }

void HermesClient::send_json(void* root_v) {
    cJSON* root = static_cast<cJSON*>(root_v);
    if (!root) return;
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return;
    if (sinks_.send_text) sinks_.send_text(sinks_.ctx, text, std::strlen(text));
    cJSON_free(text);
}

void HermesClient::show_notice(const char* text, uint32_t ttl_s, uint64_t now_ms) {
    copy_capped(view_.notice, sizeof(view_.notice), text);
    view_.notice_until_ms = now_ms + (ttl_s ? ttl_s : 8) * 1000ULL;
}

void HermesClient::send_state(uint64_t now_ms) {
    if (!welcomed_) return;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "state");
    cJSON* sensors = cJSON_AddObjectToObject(root, "sensors");
    if (battery_pct_ >= 0) cJSON_AddNumberToObject(sensors, "battery_pct", battery_pct_);
    send_json(root);
    battery_sent_ = battery_pct_;
    last_state_ms_ = now_ms;
}

void HermesClient::flush_pending_text() {
    if (!has_pending_text_ || !view_.paired) return;
    char text[sizeof(pending_text_)];
    copy_capped(text, sizeof(text), pending_text_);
    has_pending_text_ = false;
    send_chat_text(text);
}

// ---------------------------------------------------------------------------
// Transport events
// ---------------------------------------------------------------------------

void HermesClient::on_transport_open(uint64_t now_ms) {
    transport_up_ = true;
    welcomed_ = false;
    wants_reconnect_ = false;
    last_rx_ms_ = now_ms;
    view_.paired = false;
    view_.pairing_code[0] = '\0';
    if (view_.link != HermesLinkState::AuthFailed) set_link(HermesLinkState::Connecting);
    if (!has_key_) {
        copy_capped(view_.error, sizeof(view_.error), "NO DEVICE KEY");
        return;
    }
    // hello — identity, capabilities, no actions (manifest is empty), sensors.
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "proto", 1);
    cJSON_AddStringToObject(root, "device_id", device_id_);
    cJSON_AddStringToObject(root, "name", cfg_.name);
    cJSON_AddStringToObject(root, "board", cfg_.board);
    cJSON_AddStringToObject(root, "firmware", cfg_.firmware);
    cJSON* caps = cJSON_AddObjectToObject(root, "caps");
    cJSON* disp = cJSON_AddObjectToObject(caps, "display");
    cJSON_AddNumberToObject(disp, "width", cfg_.display_w);
    cJSON_AddNumberToObject(disp, "height", cfg_.display_h);
    cJSON_AddBoolToObject(disp, "color", true);
    cJSON_AddStringToObject(disp, "charset", "ascii");
    cJSON_AddNumberToObject(disp, "text_cols", 34);
    cJSON_AddNumberToObject(disp, "text_rows", 20);
    cJSON* img = cJSON_AddObjectToObject(disp, "image");
    cJSON_AddNumberToObject(img, "width", cfg_.image_max_w);
    cJSON_AddNumberToObject(img, "height", cfg_.image_max_h);
    cJSON_AddStringToObject(img, "format", "rgb565");
    cJSON* mic = cJSON_AddObjectToObject(caps, "mic");
    cJSON_AddNumberToObject(mic, "rate", 16000);
    cJSON_AddStringToObject(mic, "format", "pcm16");
    cJSON* spk = cJSON_AddObjectToObject(caps, "speaker");
    cJSON_AddNumberToObject(spk, "rate", 16000);
    cJSON_AddStringToObject(spk, "format", "pcm16");
    cJSON* inputs = cJSON_AddArrayToObject(caps, "inputs");
    cJSON_AddItemToArray(inputs, cJSON_CreateString("talk"));
    cJSON_AddItemToArray(inputs, cJSON_CreateString("cancel"));
    cJSON_AddStringToObject(caps, "talk_mode", "hold");
    cJSON* ota = cJSON_AddObjectToObject(caps, "ota");
    cJSON_AddNumberToObject(ota, "max_size", kHermesOtaMaxSize);
    cJSON_AddArrayToObject(root, "actions");
    if (battery_pct_ >= 0) {
        cJSON* sensors = cJSON_AddObjectToObject(root, "sensors");
        cJSON_AddNumberToObject(sensors, "battery_pct", battery_pct_);
    }
    send_json(root);
}

void HermesClient::on_transport_closed(uint64_t now_ms) {
    (void)now_ms;
    transport_up_ = false;
    welcomed_ = false;
    capturing_ = false;
    if (play_stream_ >= 0) {
        play_stream_ = -1;
        if (sinks_.audio_abort) sinks_.audio_abort(sinks_.ctx);
    }
    if (ota_state_ != OtaState::Idle) {
        if (sinks_.ota_abort) sinks_.ota_abort(sinks_.ctx);
        ota_reset();
    }
    view_.paired = false;
    view_.busy = false;
    if (view_.link != HermesLinkState::AuthFailed) {
        set_link(HermesLinkState::Offline);
        wants_reconnect_ = true;
    }
}

// ---------------------------------------------------------------------------
// Inbound text frames
// ---------------------------------------------------------------------------

void HermesClient::on_text(const char* json, size_t len, uint64_t now_ms) {
    last_rx_ms_ = now_ms;
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (!root) return;
    const char* type = json_str(root, "type");
    if (!type) {
        cJSON_Delete(root);
        return;
    }

    if (std::strcmp(type, "challenge") == 0) {
        const char* nonce = json_str(root, "nonce");
        bool enrolled = false;
        json_bool(root, "enrolled", &enrolled);
        if (nonce && has_key_) {
            cJSON* auth = cJSON_CreateObject();
            cJSON_AddStringToObject(auth, "type", "auth");
            if (enrolled) {
                std::string mac = hermes_auth_mac_b64(key_, device_id_, nonce);
                cJSON_AddStringToObject(auth, "mac", mac.c_str());
            } else {
                std::string key_b64 = hermes_base64_encode(key_, 32);
                cJSON_AddStringToObject(auth, "key", key_b64.c_str());
            }
            send_json(auth);
        }
    } else if (std::strcmp(type, "welcome") == 0) {
        welcomed_ = true;
        long hb = 0;
        if (json_int(root, "heartbeat_s", &hb) && hb > 0) heartbeat_s_ = static_cast<uint32_t>(hb);
        bool paired = false;
        json_bool(root, "paired", &paired);
        view_.paired = paired;
        view_.error[0] = '\0';
        set_link(paired ? HermesLinkState::Linked : HermesLinkState::Pairing);
        send_state(now_ms);
        flush_pending_text();
    } else if (std::strcmp(type, "pairing") == 0) {
        copy_capped(view_.pairing_code, sizeof(view_.pairing_code), json_str(root, "code"));
        copy_capped(view_.pairing_cmd, sizeof(view_.pairing_cmd), json_str(root, "command"));
        if (!view_.paired) set_link(HermesLinkState::Pairing);
    } else if (std::strcmp(type, "paired") == 0) {
        view_.paired = true;
        view_.pairing_code[0] = '\0';
        set_link(HermesLinkState::Linked);
        flush_pending_text();
    } else if (std::strcmp(type, "unpaired") == 0) {
        view_.paired = false;
        set_link(HermesLinkState::Pairing);
    } else if (std::strcmp(type, "ping") == 0) {
        // Answer with the server's ts when it sent one (hub echoes ours back
        // the same way); a bare pong otherwise.
        cJSON* pong = cJSON_CreateObject();
        cJSON_AddStringToObject(pong, "type", "pong");
        const cJSON* ts = cJSON_GetObjectItemCaseSensitive(root, "ts");
        if (ts && cJSON_IsNumber(ts)) cJSON_AddNumberToObject(pong, "ts", ts->valuedouble);
        send_json(pong);
    } else if (std::strcmp(type, "turn.start") == 0) {
        view_.busy = true;
    } else if (std::strcmp(type, "transcript") == 0) {
        copy_capped(view_.transcript, sizeof(view_.transcript), json_str(root, "text"));
        view_.reply[0] = '\0';
    } else if (std::strcmp(type, "status") == 0) {
        copy_capped(view_.status, sizeof(view_.status), json_str(root, "text"));
    } else if (std::strcmp(type, "reply.delta") == 0 || std::strcmp(type, "reply") == 0) {
        copy_capped(view_.reply, sizeof(view_.reply), json_str(root, "text"));
    } else if (std::strcmp(type, "turn.end") == 0) {
        view_.busy = false;
        const char* outcome = json_str(root, "outcome");
        if (outcome && std::strcmp(outcome, "success") != 0) {
            copy_capped(view_.status, sizeof(view_.status), outcome);
        } else {
            view_.status[0] = '\0';
        }
    } else if (std::strcmp(type, "notice") == 0) {
        long ttl = 8;
        json_int(root, "ttl_s", &ttl);
        show_notice(json_str(root, "text"), ttl > 0 ? static_cast<uint32_t>(ttl) : 8, now_ms);
    } else if (std::strcmp(type, "prompt") == 0) {
        copy_capped(view_.prompt_id, sizeof(view_.prompt_id), json_str(root, "id"));
        copy_capped(view_.prompt_title, sizeof(view_.prompt_title), json_str(root, "title"));
        copy_capped(view_.prompt_text, sizeof(view_.prompt_text), json_str(root, "text"));
        long ttl = 0;
        json_int(root, "ttl_s", &ttl);
        view_.prompt_ttl_s = ttl > 0 ? static_cast<uint32_t>(ttl) : 0;
        view_.prompt_since_ms = now_ms;
        view_.prompt_active = view_.prompt_id[0] != '\0';
    } else if (std::strcmp(type, "prompt.close") == 0) {
        const char* id = json_str(root, "id");
        if (!id || std::strcmp(id, view_.prompt_id) == 0) view_.prompt_active = false;
    } else if (std::strcmp(type, "display") == 0) {
        copy_capped(view_.card_title, sizeof(view_.card_title), json_str(root, "title"));
        copy_capped(view_.card_body, sizeof(view_.card_body), json_str(root, "body"));
        long ttl = 15;
        json_int(root, "ttl_s", &ttl);
        view_.card_until_ms = ttl > 0 ? now_ms + static_cast<uint64_t>(ttl) * 1000ULL : 0;
        view_.card_active = true;
    } else if (std::strcmp(type, "audio.start") == 0) {
        long stream = -1, rate = 16000;
        json_int(root, "stream", &stream);
        json_int(root, "rate", &rate);
        if (stream >= 0) {
            if (play_stream_ >= 0 && sinks_.audio_abort) sinks_.audio_abort(sinks_.ctx);
            play_stream_ = static_cast<int>(stream);
            if (sinks_.audio_begin) sinks_.audio_begin(sinks_.ctx, static_cast<uint32_t>(rate));
        }
    } else if (std::strcmp(type, "audio.end") == 0 || std::strcmp(type, "audio.abort") == 0) {
        long stream = -1;
        json_int(root, "stream", &stream);
        if (play_stream_ >= 0 && (stream < 0 || stream == play_stream_)) {
            if (sinks_.audio_end && type[6] == 'e') sinks_.audio_end(sinks_.ctx);
            if (sinks_.audio_abort && type[6] == 'a') sinks_.audio_abort(sinks_.ctx);
            play_stream_ = -1;
        }
    } else if (std::strcmp(type, "image.start") == 0) {
        long stream = -1, w = 0, h = 0, ttl = 30;
        json_int(root, "stream", &stream);
        json_int(root, "width", &w);
        json_int(root, "height", &h);
        json_int(root, "ttl_s", &ttl);
        img_stream_ = -1;
        img_received_ = 0;
        img_expected_ = 0;
        if (stream >= 0 && w > 0 && h > 0 && w <= cfg_.image_max_w && h <= cfg_.image_max_h) {
            size_t need = static_cast<size_t>(w) * static_cast<size_t>(h) * 2;
            if (!img_buf_) {
                // Big-buffer rule: platform allocator (PSRAM on device,
                // fail-closed). No buffer -> the image is dropped, frames
                // are ignored, and the conversation carries on.
                size_t cap = static_cast<size_t>(cfg_.image_max_w) *
                             static_cast<size_t>(cfg_.image_max_h) * 2;
                img_buf_ = sinks_.alloc_big
                               ? static_cast<uint8_t*>(sinks_.alloc_big(sinks_.ctx, cap))
                               : static_cast<uint8_t*>(std::malloc(cap));
            }
            if (img_buf_) {
                img_stream_ = static_cast<int>(stream);
                img_expected_ = need;
                view_.image_w = static_cast<int>(w);
                view_.image_h = static_cast<int>(h);
                view_.image_until_ms = ttl > 0 ? now_ms + static_cast<uint64_t>(ttl) * 1000ULL : 0;
                view_.image_active = false;   // shown once complete
            }
        }
    } else if (std::strcmp(type, "image.end") == 0) {
        long stream = -1;
        json_int(root, "stream", &stream);
        if (img_stream_ >= 0 && stream == img_stream_ && img_received_ >= img_expected_) {
            view_.image_active = true;
        }
        img_stream_ = -1;
    } else if (std::strcmp(type, "action") == 0) {
        // Our hello advertises no actions; answer every invocation exactly
        // once regardless, so the server never waits out its timeout.
        const char* id = json_str(root, "id");
        if (id) {
            cJSON* res = cJSON_CreateObject();
            cJSON_AddStringToObject(res, "type", "action.result");
            cJSON_AddStringToObject(res, "id", id);
            cJSON_AddBoolToObject(res, "ok", false);
            cJSON_AddStringToObject(res, "error", "unsupported action");
            send_json(res);
        }
    } else if (std::strcmp(type, "ota.offer") == 0) {
        long stream = -1, size = 0;
        json_int(root, "stream", &stream);
        json_int(root, "size", &size);
        const char* sha = json_str(root, "sha256");
        const char* version = json_str(root, "version");
        if (ota_state_ != OtaState::Idle) {
            ota_fail("sequence", "update already in progress", now_ms);
        } else if (stream < 0 || !is_sha256_hex(sha) || size <= 0) {
            ota_fail("bad_offer", "malformed offer", now_ms);
        } else if (static_cast<uint32_t>(size) > kHermesOtaMaxSize) {
            ota_fail("too_large", "image exceeds advertised max_size", now_ms);
        } else if (!sinks_.ota_begin || !sinks_.fill_random) {
            ota_fail("unsupported", "OTA not available on this device", now_ms);
        } else {
            ota_stream_ = static_cast<uint8_t>(stream);
            ota_size_ = static_cast<uint32_t>(size);
            copy_capped(ota_sha_hex_, sizeof(ota_sha_hex_), sha);
            copy_capped(ota_version_, sizeof(ota_version_), version);
            uint8_t nonce[16];
            sinks_.fill_random(sinks_.ctx, nonce, sizeof(nonce));
            std::string nonce_b64 = hermes_base64_encode(nonce, sizeof(nonce));
            copy_capped(ota_nonce_b64_, sizeof(ota_nonce_b64_), nonce_b64.c_str());
            ota_state_ = OtaState::Offered;
            ota_last_chunk_ms_ = now_ms;
            cJSON* ready = cJSON_CreateObject();
            cJSON_AddStringToObject(ready, "type", "ota.ready");
            cJSON_AddStringToObject(ready, "nonce", ota_nonce_b64_);
            send_json(ready);
        }
    } else if (std::strcmp(type, "ota.begin") == 0) {
        if (ota_state_ != OtaState::Offered) {
            ota_fail("no_offer", "ota.begin without offer", now_ms);
        } else {
            const char* mac = json_str(root, "mac");
            std::string expected = hermes_ota_mac_b64(key_, device_id_, ota_nonce_b64_,
                                                      ota_sha_hex_, ota_size_);
            if (!mac || expected.empty() || std::strcmp(mac, expected.c_str()) != 0) {
                ota_fail("unauthorized", "bad update MAC", now_ms);
            } else if (!sinks_.ota_begin(sinks_.ctx, ota_size_)) {
                ota_fail("flash", "cannot prepare update slot", now_ms);
            } else {
                ota_sha_.init();
                ota_received_ = 0;
                ota_last_ack_ = 0;
                ota_expect_seq_ = 0;
                ota_last_chunk_ms_ = now_ms;
                ota_state_ = OtaState::Receiving;
                view_.ota_active = true;
                view_.ota_received = 0;
                view_.ota_size = ota_size_;
                cJSON* ack = cJSON_CreateObject();
                cJSON_AddStringToObject(ack, "type", "ota.ack");
                cJSON_AddNumberToObject(ack, "offset", 0);
                send_json(ack);
            }
        }
    } else if (std::strcmp(type, "ota.end") == 0) {
        if (ota_state_ != OtaState::Receiving) {
            ota_fail("no_offer", "ota.end without transfer", now_ms);
        } else if (ota_received_ != ota_size_) {
            ota_fail("size", "byte count mismatch", now_ms);
        } else {
            char digest[65];
            ota_sha_.finish_hex(digest);
            bool sha_ok = true;
            for (int i = 0; i < 64; ++i) {
                if (lower_hex(digest[i]) != lower_hex(ota_sha_hex_[i])) { sha_ok = false; break; }
            }
            if (!sha_ok) {
                ota_fail("checksum", "sha256 mismatch", now_ms);
            } else if (!sinks_.ota_finish || !sinks_.ota_finish(sinks_.ctx)) {
                ota_fail("invalid", "image validation failed", now_ms);
            } else {
                cJSON* done = cJSON_CreateObject();
                cJSON_AddStringToObject(done, "type", "ota.done");
                cJSON_AddStringToObject(done, "version", ota_version_);
                send_json(done);
                ota_reset();
                ota_reboot_pending_ = true;
            }
        }
    } else if (std::strcmp(type, "ota.abort") == 0) {
        if (ota_state_ != OtaState::Idle) {
            if (sinks_.ota_abort) sinks_.ota_abort(sinks_.ctx);
            ota_reset();
        }
    } else if (std::strcmp(type, "error") == 0) {
        const char* code = json_str(root, "code");
        const char* message = json_str(root, "message");
        if (code && std::strcmp(code, "auth_failed") == 0) {
            set_link(HermesLinkState::AuthFailed);
            wants_reconnect_ = false;
            copy_capped(view_.error, sizeof(view_.error),
                        "AUTH FAILED - FORGET + RE-ENROLL");
        } else if (message) {
            show_notice(message, 8, now_ms);
        }
    }
    // Unknown types are ignored by design (protocol versioning rule).

    cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// Inbound binary frames
// ---------------------------------------------------------------------------

void HermesClient::on_binary(const uint8_t* data, size_t len, uint64_t now_ms) {
    last_rx_ms_ = now_ms;
    uint8_t channel = 0, stream = 0;
    uint16_t seq = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    if (!hermes_binary_parse(data, len, &channel, &stream, &seq, &payload, &payload_len)) return;

    if (channel == kHermesChannelAudio) {
        if (play_stream_ >= 0 && stream == static_cast<uint8_t>(play_stream_) && sinks_.audio_write) {
            sinks_.audio_write(sinks_.ctx, reinterpret_cast<const int16_t*>(payload),
                               payload_len / 2);
        }
    } else if (channel == kHermesChannelImage) {
        if (img_stream_ >= 0 && stream == static_cast<uint8_t>(img_stream_) && img_buf_) {
            size_t room = img_expected_ > img_received_ ? img_expected_ - img_received_ : 0;
            size_t take = payload_len < room ? payload_len : room;
            if (take) std::memcpy(img_buf_ + img_received_, payload, take);
            img_received_ += take;
        }
    } else if (channel == kHermesChannelFirmware) {
        if (ota_state_ != OtaState::Receiving || stream != ota_stream_) return;
        if (seq != ota_expect_seq_) {
            ota_fail("sequence", "firmware frame gap", now_ms);
            return;
        }
        if (!sinks_.ota_write || !sinks_.ota_write(sinks_.ctx, payload, payload_len)) {
            ota_fail("flash", "write failed", now_ms);
            return;
        }
        ota_sha_.update(payload, payload_len);
        ota_received_ += static_cast<uint32_t>(payload_len);
        ++ota_expect_seq_;
        ota_last_chunk_ms_ = now_ms;
        view_.ota_received = ota_received_;
        if (ota_received_ - ota_last_ack_ >= 16384 || ota_received_ == ota_size_) {
            ota_last_ack_ = ota_received_;
            cJSON* ack = cJSON_CreateObject();
            cJSON_AddStringToObject(ack, "type", "ota.ack");
            cJSON_AddNumberToObject(ack, "offset", ota_received_);
            send_json(ack);
        }
    }
}

// ---------------------------------------------------------------------------
// OTA helpers
// ---------------------------------------------------------------------------

void HermesClient::ota_reset() {
    ota_state_ = OtaState::Idle;
    view_.ota_active = false;
}

void HermesClient::ota_fail(const char* code, const char* message, uint64_t now_ms) {
    (void)now_ms;
    if (ota_state_ == OtaState::Receiving && sinks_.ota_abort) sinks_.ota_abort(sinks_.ctx);
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "type", "ota.error");
    cJSON_AddStringToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    send_json(err);
    ota_reset();
}

// ---------------------------------------------------------------------------
// User intents
// ---------------------------------------------------------------------------

bool HermesClient::send_chat_text(const char* text) {
    if (!text || !text[0]) return false;
    if (!welcomed_ || !view_.paired) {
        copy_capped(pending_text_, sizeof(pending_text_), text);
        has_pending_text_ = true;
        return true;   // queued; flushed on welcome/paired
    }
    char id[16];
    std::snprintf(id, sizeof(id), "t%u", ++msg_counter_);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "text");
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "text", text);
    send_json(root);
    return true;
}

bool HermesClient::answer_prompt(bool yes) {
    if (!view_.prompt_active) return false;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "prompt.reply");
    cJSON_AddStringToObject(root, "id", view_.prompt_id);
    cJSON_AddStringToObject(root, "answer", yes ? "yes" : "no");
    send_json(root);
    view_.prompt_active = false;
    return true;
}

void HermesClient::dismiss_card() {
    view_.card_active = false;
    view_.image_active = false;
}

void HermesClient::cancel(uint64_t now_ms) {
    (void)now_ms;
    if (capturing_) {
        talk_cancel();   // swipe-down while holding talk discards the utterance
        return;
    }
    if (view_.busy) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "cancel");
        send_json(root);
    }
}

bool HermesClient::new_session(uint64_t now_ms) {
    (void)now_ms;
    if (!welcomed_ || !view_.paired) return false;   // can_talk gate
    if (capturing_) talk_cancel();
    if (play_stream_ >= 0) {
        if (sinks_.audio_abort) sinks_.audio_abort(sinks_.ctx);
        play_stream_ = -1;
    }
    view_.prompt_active = false;
    dismiss_card();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.new");
    send_json(root);
    view_.transcript[0] = '\0';
    view_.reply[0] = '\0';
    copy_capped(view_.status, sizeof(view_.status), "Starting a new session");
    view_.busy = true;
    return true;
}

bool HermesClient::talk_start(uint64_t now_ms) {
    (void)now_ms;
    if (capturing_ || !welcomed_ || !view_.paired) return false;
    talk_stream_ = static_cast<uint8_t>(1 + (msg_counter_ % 250));
    std::snprintf(talk_id_, sizeof(talk_id_), "a%u", ++msg_counter_);
    talk_seq_ = 0;
    talk_samples_ = 0;
    talk_fill_ = 0;
    capturing_ = true;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "audio.start");
    cJSON_AddStringToObject(root, "id", talk_id_);
    cJSON_AddNumberToObject(root, "stream", talk_stream_);
    cJSON_AddNumberToObject(root, "rate", 16000);
    cJSON_AddStringToObject(root, "format", "pcm16");
    cJSON_AddStringToObject(root, "mode", "hold");
    send_json(root);
    return true;
}

void HermesClient::talk_feed(const int16_t* pcm, size_t frames) {
    if (!capturing_ || !pcm) return;
    while (frames > 0) {
        size_t room = 640 - talk_fill_;
        size_t take = frames < room ? frames : room;
        std::memcpy(talk_buf_ + talk_fill_, pcm, take * sizeof(int16_t));
        talk_fill_ += take;
        talk_samples_ += static_cast<uint32_t>(take);
        pcm += take;
        frames -= take;
        if (talk_fill_ == 640) {
            uint8_t frame[kHermesBinaryHeader + 640 * 2];
            hermes_binary_header_pack(frame, kHermesChannelAudio, talk_stream_, talk_seq_++);
            std::memcpy(frame + kHermesBinaryHeader, talk_buf_, 640 * sizeof(int16_t));
            if (sinks_.send_binary) {
                sinks_.send_binary(sinks_.ctx, frame, sizeof(frame));
            }
            talk_fill_ = 0;
        }
    }
}

void HermesClient::talk_stop(uint64_t now_ms) {
    (void)now_ms;
    if (!capturing_) return;
    capturing_ = false;
    if (talk_fill_ > 0) {
        uint8_t frame[kHermesBinaryHeader + 640 * 2];
        hermes_binary_header_pack(frame, kHermesChannelAudio, talk_stream_, talk_seq_++);
        std::memcpy(frame + kHermesBinaryHeader, talk_buf_, talk_fill_ * sizeof(int16_t));
        if (sinks_.send_binary) {
            sinks_.send_binary(sinks_.ctx, frame, kHermesBinaryHeader + talk_fill_ * sizeof(int16_t));
        }
        talk_fill_ = 0;
    }
    uint32_t duration_ms = talk_samples_ * 1000 / 16000;
    cJSON* root = cJSON_CreateObject();
    if (duration_ms < 250) {
        cJSON_AddStringToObject(root, "type", "audio.cancel");
        cJSON_AddStringToObject(root, "reason", "too short");
    } else {
        cJSON_AddStringToObject(root, "type", "audio.end");
        cJSON_AddNumberToObject(root, "duration_ms", duration_ms);
    }
    cJSON_AddStringToObject(root, "id", talk_id_);
    cJSON_AddNumberToObject(root, "stream", talk_stream_);
    send_json(root);
}

void HermesClient::talk_cancel() {
    if (!capturing_) return;
    capturing_ = false;
    talk_fill_ = 0;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "audio.cancel");
    cJSON_AddStringToObject(root, "id", talk_id_);
    cJSON_AddNumberToObject(root, "stream", talk_stream_);
    cJSON_AddStringToObject(root, "reason", "cancelled");
    send_json(root);
}

void HermesClient::set_battery(int pct, uint64_t now_ms) {
    battery_pct_ = pct;
    if (!welcomed_) return;
    bool changed = battery_sent_ >= 0 && pct >= 0 && (pct - battery_sent_ >= 2 || battery_sent_ - pct >= 2);
    bool stale = now_ms - last_state_ms_ >= 60000;
    if (changed || stale || (battery_sent_ < 0 && pct >= 0)) send_state(now_ms);
}

// ---------------------------------------------------------------------------
// Periodic drive
// ---------------------------------------------------------------------------

void HermesClient::tick(uint64_t now_ms) {
    // Heartbeat: 3 missed intervals with zero inbound frames = dead link.
    if (transport_up_ && welcomed_ &&
        now_ms - last_rx_ms_ > static_cast<uint64_t>(heartbeat_s_) * 3000ULL) {
        transport_up_ = false;
        welcomed_ = false;
        view_.paired = false;
        view_.busy = false;
        if (view_.link != HermesLinkState::AuthFailed) {
            set_link(HermesLinkState::Offline);
            wants_reconnect_ = true;
        }
    }
    // TTL expiry.
    if (view_.notice_until_ms && now_ms >= view_.notice_until_ms) {
        view_.notice[0] = '\0';
        view_.notice_until_ms = 0;
    }
    if (view_.card_active && view_.card_until_ms && now_ms >= view_.card_until_ms) {
        view_.card_active = false;
    }
    if (view_.image_active && view_.image_until_ms && now_ms >= view_.image_until_ms) {
        view_.image_active = false;
    }
    if (view_.prompt_active && view_.prompt_ttl_s &&
        now_ms - view_.prompt_since_ms >= static_cast<uint64_t>(view_.prompt_ttl_s) * 1000ULL) {
        view_.prompt_active = false;   // expires unanswered, per protocol
    }
    // OTA stall: 30 s without a firmware chunk abandons the update.
    if (ota_state_ == OtaState::Receiving && now_ms - ota_last_chunk_ms_ > 30000) {
        ota_fail("timeout", "no firmware data for 30s", now_ms);
    } else if (ota_state_ == OtaState::Offered && now_ms - ota_last_chunk_ms_ > 60000) {
        ota_reset();
    }
}

}  // namespace aiwatchos_hermes
