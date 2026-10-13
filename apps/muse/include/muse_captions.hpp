// Caption + activity text for the Muse face — faithful C++ ports of
// MuseCompanionApp's app/lib/app/captions.dart and the pose mapper in
// app/lib/app/avatar_motion.dart. Pure text in, pure text out: no
// network, no display, host-testable.
//
// Divergences from the Dart, all deliberate and documented:
//   - Lengths count UTF-8 code points where Dart counts UTF-16 code
//     units (identical for the BMP text replies actually carry).
//   - Whitespace/case handling is ASCII where Dart is Unicode-aware.
//   - activityCaption() is ported as the companion CALLS it — with an
//     empty argument map (captions.dart: toolLabel(key, const {})) — so
//     argument-dependent labels reduce to their empty-argument results
//     ("Opening …", "Browsing …"). The wire payload the watch parses
//     carries a code, not tool arguments.
#pragma once

#include <cctype>
#include <cstddef>
#include <string>

namespace aiwatchos_muse {

// Visible caption limit under the character (captions.dart captionLimit).
inline constexpr size_t kCaptionLimit = 240;

namespace captions_detail {

inline bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

// Number of UTF-8 code points in s (invalid bytes count as one each).
inline size_t utf8_len(const std::string& s) {
    size_t n = 0;
    for (char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

// Byte prefix of s holding at most `cp` code points.
inline std::string utf8_prefix(const std::string& s, size_t cp) {
    size_t seen = 0;
    size_t i = 0;
    while (i < s.size() && seen < cp) {
        ++seen;
        ++i;
        while (i < s.size() &&
               (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
            ++i;
        }
    }
    return s.substr(0, i);
}

inline std::string trim_right_ws(std::string s) {
    while (!s.empty() && is_ws(s.back())) s.pop_back();
    return s;
}

// captions.dart _clean(): fenced code blocks -> one space, markdown
// links -> their text, markdown chars removed, whitespace collapsed.
inline std::string clean(std::string value) {
    // 1. ```...``` (non-greedy) -> ' '. An unclosed fence does not
    // match the Dart regex either; its backticks fall to step 3.
    for (size_t open = value.find("```"); open != std::string::npos;) {
        size_t close = value.find("```", open + 3);
        if (close == std::string::npos) break;
        value.replace(open, close + 3 - open, " ");
        open = value.find("```", open + 1);
    }
    // 2. [text](url) -> text. Regex shape: \[([^\]]+)\]\([^)]+\).
    for (size_t i = 0; i < value.size();) {
        if (value[i] != '[') { ++i; continue; }
        size_t rb = value.find(']', i + 1);
        if (rb == std::string::npos || rb == i + 1) { ++i; continue; }
        if (rb + 1 >= value.size() || value[rb + 1] != '(') { ++i; continue; }
        size_t rp = value.find(')', rb + 2);
        if (rp == std::string::npos || rp == rb + 2) { ++i; continue; }
        std::string text = value.substr(i + 1, rb - i - 1);
        value.replace(i, rp + 1 - i, text);
        i += text.size();
    }
    // 3. Remove every [*_~`#>].
    std::string stripped;
    stripped.reserve(value.size());
    for (char c : value) {
        if (c == '*' || c == '_' || c == '~' || c == '`' || c == '#' ||
            c == '>') {
            continue;
        }
        stripped.push_back(c);
    }
    // 4. Collapse whitespace runs to one space; trim both ends.
    std::string out;
    out.reserve(stripped.size());
    bool pending_space = false;
    for (char c : stripped) {
        if (is_ws(c)) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(c);
    }
    return out;
}

// captions.dart _clip(): over-limit text is cut to limit-1 and closed
// with an ellipsis, right-trimmed first.
inline std::string clip(const std::string& text, size_t limit) {
    if (utf8_len(text) <= limit) return text;
    std::string cut = trim_right_ws(utf8_prefix(text, limit - 1));
    cut += "\xE2\x80\xA6";   // U+2026 HORIZONTAL ELLIPSIS
    return cut;
}

// captions.dart key normalization for activity codes.
inline std::string activity_key(const char* code) {
    if (!code) return "";
    std::string s(code);
    size_t b = 0, e = s.size();
    while (b < e && is_ws(s[b])) ++b;
    while (e > b && is_ws(s[e - 1])) --e;
    std::string key = s.substr(b, e - b);
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == ' ') c = '_';
    }
    return key;
}

}  // namespace captions_detail

// Plain caption from a Muse reply, markdown removed, clipped to
// kCaptionLimit (captions.dart captionFromReply).
inline std::string caption_from_reply(const std::string& text) {
    return captions_detail::clip(captions_detail::clean(text), kCaptionLimit);
}

// The same cleaning WITHOUT the caption clip — for speech (M10): the
// TTS engine chunks the full text itself, so the 240-char display
// limit must not truncate what gets spoken.
inline std::string caption_clean_full(const std::string& text) {
    return captions_detail::clean(text);
}

// Short line for an agent.status activity code (captions.dart
// activityCaption). Returns false when the code is empty/unusable.
inline bool activity_caption(const char* code, std::string* out) {
    if (!out) return false;
    const std::string key = captions_detail::activity_key(code);
    if (key.empty()) return false;
    // Phone-tool labels (agent_status.dart toolLabel with empty args).
    static const struct { const char* key; const char* label; } kToolLabels[] = {
        {"take_photo", "Taking a photo\xE2\x80\xA6"},
        {"speak_text", "Speaking\xE2\x80\xA6"},
        {"get_device_health", "Checking device health\xE2\x80\xA6"},
        {"get_location", "Getting location\xE2\x80\xA6"},
        {"list_notifications", "Listing notifications\xE2\x80\xA6"},
        {"set_alarm", "Setting an alarm\xE2\x80\xA6"},
        {"set_timer", "Setting a timer\xE2\x80\xA6"},
        {"show_notification", "Showing a notification\xE2\x80\xA6"},
        {"open_url", "Opening \xE2\x80\xA6"},
        {"launch_app", "Opening \xE2\x80\xA6"},
        {"vibrate", "Vibrating\xE2\x80\xA6"},
        {"get_clipboard", "Reading the clipboard\xE2\x80\xA6"},
        {"set_clipboard", "Copying to the clipboard\xE2\x80\xA6"},
        {"toggle_flashlight", "Toggling the flashlight\xE2\x80\xA6"},
        {"usb_list_devices", "Listing USB devices\xE2\x80\xA6"},
        {"usb_list_volumes", "Browsing USB storage\xE2\x80\xA6"},
        {"usb_list_files", "Browsing \xE2\x80\xA6"},
        {"usb_serial_list", "Listing serial ports\xE2\x80\xA6"},
    };
    for (const auto& t : kToolLabels) {
        if (key == t.key) { *out = t.label; return true; }
    }
    static const struct { const char* key; const char* label; } kKnown[] = {
        {"thinking", "Thinking\xE2\x80\xA6"},
        {"working", "Working\xE2\x80\xA6"},
        {"searching", "Searching\xE2\x80\xA6"},
        {"reading", "Reading\xE2\x80\xA6"},
        {"writing", "Writing\xE2\x80\xA6"},
        {"listening", "Listening\xE2\x80\xA6"},
        {"speaking", "Speaking\xE2\x80\xA6"},
        {"browsing", "Looking something up\xE2\x80\xA6"},
        {"using_tool", "Using a tool\xE2\x80\xA6"},
        {"tool", "Using a tool\xE2\x80\xA6"},
        {"waiting", "Waiting\xE2\x80\xA6"},
    };
    for (const auto& t : kKnown) {
        if (key == t.key) { *out = t.label; return true; }
    }
    // Fallback: the code's words, first letter upper-cased.
    std::string words;
    size_t i = 0;
    while (i < key.size()) {
        while (i < key.size() && key[i] == '_') ++i;
        size_t j = i;
        while (j < key.size() && key[j] != '_') ++j;
        if (j > i) {
            if (!words.empty()) words.push_back(' ');
            words += key.substr(i, j - i);
        }
        i = j;
    }
    if (words.empty()) return false;
    if (words[0] >= 'a' && words[0] <= 'z') {
        words[0] = static_cast<char>(words[0] - 'a' + 'A');
    }
    *out = words;
    return true;
}

// Pose for an activity code (avatar_motion.dart poseForActivity).
enum class ActivityPose : uint8_t {
    kIdle, kListening, kThinking, kSpeaking, kError, kBoot, kOff
};

inline ActivityPose activity_pose(const char* code) {
    const std::string key = captions_detail::activity_key(code);
    if (key == "listening") return ActivityPose::kListening;
    if (key == "speaking") return ActivityPose::kSpeaking;
    if (key == "error" || key == "failed") return ActivityPose::kError;
    if (key == "off" || key == "shutdown" || key == "asleep" ||
        key == "sleep") {
        return ActivityPose::kOff;
    }
    if (key == "boot" || key == "starting") return ActivityPose::kBoot;
    if (key.empty() || key == "idle" || key == "waiting") {
        return ActivityPose::kIdle;
    }
    return ActivityPose::kThinking;
}

}  // namespace aiwatchos_muse
