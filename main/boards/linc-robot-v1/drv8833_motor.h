#ifndef DRV8833_MOTOR_H
#define DRV8833_MOTOR_H

#include <driver/gpio.h>
#include <driver/ledc.h>

// Two DC motors on a DRV8833. The chip has no enable pins, so each channel is driven by two
// LEDC PWM outputs: PWM on IN1 (IN2 low) turns the motor one way, PWM on IN2 (IN1 low) the
// other way, both low coasts to a stop. Channel A is the left wheel, channel B the right.
class Drv8833Motor {
public:
    Drv8833Motor(gpio_num_t a_in1, gpio_num_t a_in2, gpio_num_t b_in1, gpio_num_t b_in2,
                 bool left_invert = false, bool right_invert = false);

    // Speeds are -100..100 (negative = reverse). Values outside the range are clamped.
    void Drive(int left, int right);
    void Stop();

private:
    struct Side {
        ledc_channel_t forward;
        ledc_channel_t reverse;
        bool invert;
    };
    Side left_;
    Side right_;

    static void InitChannel(ledc_channel_t channel, gpio_num_t pin);
    static void SetSide(const Side& side, int speed);
};

#endif  // DRV8833_MOTOR_H
