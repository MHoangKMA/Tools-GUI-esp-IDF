#include "drv8833_motor.h"

#include <algorithm>
#include <cstdlib>

#include <esp_check.h>
#include <esp_log.h>

#define TAG "Drv8833Motor"

namespace {
constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_3;
constexpr ledc_timer_bit_t kResolution = LEDC_TIMER_8_BIT;
constexpr uint32_t kMaxDuty = (1u << 8) - 1;
constexpr uint32_t kPwmFreqHz = 20000;  // above the audible range, no motor whine
}  // namespace

Drv8833Motor::Drv8833Motor(gpio_num_t a_in1, gpio_num_t a_in2, gpio_num_t b_in1, gpio_num_t b_in2,
                           bool left_invert, bool right_invert)
    : left_{LEDC_CHANNEL_4, LEDC_CHANNEL_5, left_invert},
      right_{LEDC_CHANNEL_6, LEDC_CHANNEL_7, right_invert} {
    ledc_timer_config_t timer = {};
    timer.speed_mode = kSpeedMode;
    timer.duty_resolution = kResolution;
    timer.timer_num = kTimer;
    timer.freq_hz = kPwmFreqHz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    InitChannel(left_.forward, a_in1);
    InitChannel(left_.reverse, a_in2);
    InitChannel(right_.forward, b_in1);
    InitChannel(right_.reverse, b_in2);
    Stop();
}

void Drv8833Motor::InitChannel(ledc_channel_t channel, gpio_num_t pin) {
    ledc_channel_config_t config = {};
    config.gpio_num = pin;
    config.speed_mode = kSpeedMode;
    config.channel = channel;
    config.timer_sel = kTimer;
    config.duty = 0;
    config.hpoint = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&config));
}

void Drv8833Motor::SetSide(const Side& side, int speed) {
    speed = std::clamp(speed, -100, 100);
    if (side.invert) {
        speed = -speed;
    }
    uint32_t duty = static_cast<uint32_t>(std::abs(speed)) * kMaxDuty / 100;
    // Only one input carries PWM at a time, the other is held low
    ledc_set_duty(kSpeedMode, side.forward, speed > 0 ? duty : 0);
    ledc_set_duty(kSpeedMode, side.reverse, speed < 0 ? duty : 0);
    ledc_update_duty(kSpeedMode, side.forward);
    ledc_update_duty(kSpeedMode, side.reverse);
}

void Drv8833Motor::Drive(int left, int right) {
    SetSide(left_, left);
    SetSide(right_, right);
}

void Drv8833Motor::Stop() { Drive(0, 0); }
