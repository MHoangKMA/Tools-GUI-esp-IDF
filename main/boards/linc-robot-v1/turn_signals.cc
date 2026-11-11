#include "turn_signals.h"

#include <esp_check.h>
#include <esp_log.h>

#define TAG "TurnSignals"

TurnSignals& TurnSignals::GetInstance() {
    static TurnSignals instance;
    return instance;
}

void TurnSignals::Initialize(gpio_num_t left, gpio_num_t right, bool active_low) {
    left_ = left;
    right_ = right;
    active_low_ = active_low;

    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << left_) | (1ULL << right_);
    io.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&io));
    Drive(false, false);

    esp_timer_create_args_t args = {};
    args.callback = TimerCallback;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "turn_signals";
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));
}

void TurnSignals::Drive(bool left, bool right) {
    gpio_set_level(left_, left != active_low_ ? 1 : 0);
    gpio_set_level(right_, right != active_low_ ? 1 : 0);
}

SignalPattern TurnSignals::ChooseLocked(int64_t now_us) const {
    if (manual_ != SignalPattern::kOff && now_us < manual_until_us_) {
        return manual_;
    }
    if (movement_ != SignalPattern::kOff) {
        return movement_;
    }
    if (effect_ != SignalPattern::kOff && now_us < effect_until_us_) {
        return effect_;
    }
    return SignalPattern::kOff;
}

void TurnSignals::Kick() {
    // Starting a timer that already runs only returns ESP_ERR_INVALID_STATE
    esp_timer_start_periodic(timer_, static_cast<uint64_t>(kSignalTickMs) * 1000);
}

void TurnSignals::SetMovement(SignalPattern pattern) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        movement_ = pattern;
    }
    Kick();
}

void TurnSignals::SetManual(SignalPattern pattern, int duration_ms) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pattern == SignalPattern::kOff || duration_ms <= 0) {
            manual_ = SignalPattern::kOff;
            effect_ = SignalPattern::kOff;  // "lights off" also ends a running light show
        } else {
            manual_ = pattern;
            manual_until_us_ = esp_timer_get_time() + static_cast<int64_t>(duration_ms) * 1000;
        }
    }
    Kick();
}

void TurnSignals::SetEffect(SignalPattern pattern, int duration_ms) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        effect_ = duration_ms > 0 ? pattern : SignalPattern::kOff;
        effect_until_us_ = esp_timer_get_time() + static_cast<int64_t>(duration_ms) * 1000;
    }
    Kick();
}

void TurnSignals::OnEmotion(const std::string& emotion) {
    if (!effects_enabled_) {
        return;
    }
    EmotionEffect effect = EffectForEmotion(emotion);
    SetEffect(effect.pattern, effect.duration_ms);  // a calm face (kOff) ends the previous show
}

SignalPattern TurnSignals::Current() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ChooseLocked(esp_timer_get_time());
}

void TurnSignals::TimerCallback(void* arg) { static_cast<TurnSignals*>(arg)->Tick(); }

void TurnSignals::Tick() {
    SignalState state{false, false};
    bool idle = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SignalPattern pattern = ChooseLocked(esp_timer_get_time());
        if (pattern != shown_) {
            shown_ = pattern;
            phase_ = 0;  // a new pattern always starts with the light on
        }
        state = SignalAt(pattern, phase_++);
        idle = pattern == SignalPattern::kOff;
        if (idle) {
            esp_timer_stop(timer_);  // nothing blinks: stop waking the CPU
        }
    }
    Drive(state.left, state.right);
}
