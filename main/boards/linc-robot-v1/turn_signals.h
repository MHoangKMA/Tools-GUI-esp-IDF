#ifndef TURN_SIGNALS_H
#define TURN_SIGNALS_H

#include <mutex>
#include <string>

#include <driver/gpio.h>
#include <esp_timer.h>

#include "signal_pattern.h"

// The two LEDs used as turn signals. Three layers decide what blinks, the highest active one wins:
//   1. manual    a command ("blink the left light for 10 s"), with a duration
//   2. movement  follows the wheels: left turn = left light, right turn = right light, straight
//                forward / backward = both; set by RobotController, ends when the robot stops
//   3. effect    a light show that goes with a face (angry = fast flashes, happy = alternating ...),
//                runs for a few seconds, never overrides the movement signals
// The 50 ms timer runs only while something is blinking, so an idle robot can still sleep.
class TurnSignals {
public:
    static TurnSignals& GetInstance();

    // `active_low` = the LED lights when the pin is low. Swap `left` and `right` in the board
    // config if the lights are on the other sides.
    void Initialize(gpio_num_t left, gpio_num_t right, bool active_low = false);

    void SetMovement(SignalPattern pattern);
    void SetManual(SignalPattern pattern, int duration_ms);  // kOff or duration <= 0 cancels it
    void SetEffect(SignalPattern pattern, int duration_ms);
    void OnEmotion(const std::string& emotion);               // starts / cancels the effect layer
    void SetEffectsEnabled(bool enabled) { effects_enabled_ = enabled; }

    SignalPattern Current();  // what blinks right now

private:
    TurnSignals() = default;

    static void TimerCallback(void* arg);
    void Tick();
    void Kick();
    SignalPattern ChooseLocked(int64_t now_us) const;
    void Drive(bool left, bool right);

    gpio_num_t left_ = GPIO_NUM_NC;
    gpio_num_t right_ = GPIO_NUM_NC;
    bool active_low_ = false;
    bool effects_enabled_ = true;
    esp_timer_handle_t timer_ = nullptr;

    std::mutex mutex_;
    SignalPattern movement_ = SignalPattern::kOff;
    SignalPattern manual_ = SignalPattern::kOff;
    int64_t manual_until_us_ = 0;
    SignalPattern effect_ = SignalPattern::kOff;
    int64_t effect_until_us_ = 0;
    SignalPattern shown_ = SignalPattern::kOff;
    int phase_ = 0;
};

#endif  // TURN_SIGNALS_H
