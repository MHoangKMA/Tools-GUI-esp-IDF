#ifndef SIGNAL_PATTERN_H
#define SIGNAL_PATTERN_H

// Blink patterns of the two signal lights and the rules that choose them. Plain C++, no ESP-IDF
// headers, so it can be unit-tested on a PC; TurnSignals (turn_signals.*) drives the GPIOs.

#include <string>

enum class SignalPattern {
    kOff,
    kLeft,       // left light blinks (turning left)
    kRight,      // right light blinks (turning right)
    kBoth,       // both blink together (driving straight, hazard)
    kAlternate,  // left and right take turns
    kFastBoth,   // both blink fast
    kSlowBoth,   // both blink slowly
};

struct SignalState {
    bool left;
    bool right;
};

constexpr int kSignalTickMs = 50;  // the unit of `tick` below

// State of the lights `tick` ticks after the pattern started. Every blinking pattern starts with
// the light(s) ON.  Normal blink: 250 ms on / 250 ms off (2 Hz), fast 100 / 100, slow 500 / 500.
SignalState SignalAt(SignalPattern pattern, int tick);

// Pattern that goes with the wheel speeds (-100..100 each): both wheels the same way = both
// lights, the right wheel faster = turning left, the left wheel faster = turning right,
// stopped = off.
SignalPattern MovementPattern(int left_speed, int right_speed);

// "left", "right", "both" / "hazard", "alternate", "fast", "slow", "off" (also "trai", "phai").
bool ParseSignalMode(const std::string& text, SignalPattern* pattern);
const char* SignalName(SignalPattern pattern);

// Light show that goes with a face. `pattern` == kOff means no effect for that emotion.
struct EmotionEffect {
    SignalPattern pattern;
    int duration_ms;
};
EmotionEffect EffectForEmotion(const std::string& emotion);

#endif  // SIGNAL_PATTERN_H
