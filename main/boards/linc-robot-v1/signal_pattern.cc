#include "signal_pattern.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>

namespace {

// Ticks of one half period (light on, or light off)
int HalfPeriod(SignalPattern pattern) {
    switch (pattern) {
        case SignalPattern::kFastBoth:
            return 2;   // 100 ms
        case SignalPattern::kSlowBoth:
            return 10;  // 500 ms
        default:
            return 5;   // 250 ms
    }
}

}  // namespace

SignalState SignalAt(SignalPattern pattern, int tick) {
    if (pattern == SignalPattern::kOff) {
        return {false, false};
    }
    const bool first_half = (std::max(tick, 0) / HalfPeriod(pattern)) % 2 == 0;
    switch (pattern) {
        case SignalPattern::kLeft:
            return {first_half, false};
        case SignalPattern::kRight:
            return {false, first_half};
        case SignalPattern::kAlternate:
            return {first_half, !first_half};
        default:  // kBoth, kFastBoth, kSlowBoth
            return {first_half, first_half};
    }
}

SignalPattern MovementPattern(int left_speed, int right_speed) {
    if (left_speed == 0 && right_speed == 0) {
        return SignalPattern::kOff;
    }
    if (left_speed < right_speed) {
        return SignalPattern::kLeft;   // the right wheel pushes harder: the robot turns left
    }
    if (left_speed > right_speed) {
        return SignalPattern::kRight;
    }
    return SignalPattern::kBoth;       // straight, forward or backward
}

bool ParseSignalMode(const std::string& text, SignalPattern* pattern) {
    std::string lower;
    for (unsigned char c : text) {
        lower += static_cast<char>(std::tolower(c));
    }
    struct Entry {
        const char* name;
        SignalPattern pattern;
    };
    static const Entry entries[] = {
        {"left", SignalPattern::kLeft},       {"trai", SignalPattern::kLeft},
        {"right", SignalPattern::kRight},     {"phai", SignalPattern::kRight},
        {"both", SignalPattern::kBoth},       {"hazard", SignalPattern::kBoth},
        {"alternate", SignalPattern::kAlternate},
        {"fast", SignalPattern::kFastBoth},   {"slow", SignalPattern::kSlowBoth},
        {"off", SignalPattern::kOff},
    };
    for (const auto& entry : entries) {
        if (lower == entry.name) {
            if (pattern) {
                *pattern = entry.pattern;
            }
            return true;
        }
    }
    return false;
}

const char* SignalName(SignalPattern pattern) {
    switch (pattern) {
        case SignalPattern::kLeft: return "left";
        case SignalPattern::kRight: return "right";
        case SignalPattern::kBoth: return "both";
        case SignalPattern::kAlternate: return "alternate";
        case SignalPattern::kFastBoth: return "fast";
        case SignalPattern::kSlowBoth: return "slow";
        default: return "off";
    }
}

EmotionEffect EffectForEmotion(const std::string& emotion) {
    const char* e = emotion.c_str();
    auto is = [e](std::initializer_list<const char*> names) {
        for (const char* name : names) {
            if (strcmp(e, name) == 0) {
                return true;
            }
        }
        return false;
    };
    if (is({"happy", "smile", "laughing", "joy", "funny", "silly", "excited", "dizzy", "dead"})) {
        return {SignalPattern::kAlternate, 3000};   // a little celebration
    }
    if (is({"loving", "heart", "love", "kissy", "embarrassed", "shy", "winking", "wink", "delicious",
            "yummy"})) {
        return {SignalPattern::kSlowBoth, 3000};    // soft pulse
    }
    if (is({"angry", "mad"})) {
        return {SignalPattern::kFastBoth, 3000};    // warning flashes
    }
    if (is({"surprised", "wow", "shocked", "scared"})) {
        return {SignalPattern::kFastBoth, 600};     // two or three quick flashes
    }
    if (is({"sad", "crying", "cry", "worried"})) {
        return {SignalPattern::kSlowBoth, 2000};
    }
    return {SignalPattern::kOff, 0};                // calm faces: no effect
}
