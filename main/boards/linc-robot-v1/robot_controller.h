#ifndef ROBOT_CONTROLLER_H
#define ROBOT_CONTROLLER_H

#include <atomic>
#include <mutex>
#include <string>

#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "drv8833_motor.h"

// Wheel control shared by the MCP tools (voice) and the web page.
//
// Safety: every move carries a duration and the wheels slow down and stop by themselves when it
// elapses, so a lost Wi-Fi link or a finger lifted from the web D-pad never leaves the robot
// driving. Stop() cuts the motors at once.
//
// Turn signals: the lights follow the wheel targets (TurnSignals::SetMovement): left turn = left
// light, right turn = right light, forward / backward = both; they go off when the robot stops.
//
// Soft start: speed changes follow a ramp (12 % every 20 ms, full speed in about 170 ms). Motors
// that jump from rest to full power, or reverse in one step, pull a current spike from a small
// battery / USB supply and can reset the ESP32; a reversal passes through zero on the ramp.
class RobotController {
public:
    static RobotController& GetInstance();

    // Called once by the board
    void Initialize(Drv8833Motor* motor);
    void RegisterMcpTools();

    // direction: forward, backward, left, right (spin in place). speed 0-100, duration in ms.
    bool Move(const std::string& direction, int speed, int duration_ms, std::string* error = nullptr);
    // Exact moves, open loop (no encoders): the time is computed from calibrated speeds.
    // direction forward / backward + distance in cm (1-300); direction left / right + angle in degrees (1-360).
    bool MoveDistance(const std::string& direction, int cm, int speed, std::string* error = nullptr);
    bool Turn(const std::string& direction, int degrees, int speed, std::string* error = nullptr);

    // Calibration: kind 0 = straight line, 1 = spin. RunTest drives a known time at a known speed; the user measures
    // what the robot really did and SaveCalibration turns the measurement into cm/s (or deg/s) at full speed.
    bool RunTest(int kind, std::string* error = nullptr);
    bool SaveCalibration(int kind, float measured, std::string* error = nullptr);
    float cm_per_s_full() const { return cm_per_s_full_; }
    float deg_per_s_full() const { return deg_per_s_full_; }

    void Stop();                 // immediate, no ramp
    void Dance(int speed = -1);  // speed < 0 keeps the current default speed
    bool IsMoving() const { return moving_.load(); }
    int default_speed() const { return default_speed_; }
    void set_default_speed(int speed);

    static constexpr int kMaxDurationMs = 5000;
    static constexpr int kMaxDistanceMs = 12000;  // longest run of an exact move

private:
    RobotController() = default;

    static void StopTimerCallback(void* arg);
    static void RampTimerCallback(void* arg);
    static void DanceTask(void* arg);

    bool MoveLimited(const std::string& direction, int speed, int duration_ms, int max_ms, std::string* error);
    void SetTarget(int left, int right);  // ramps towards the new speeds
    void RampStep();

    Drv8833Motor* motor_ = nullptr;
    esp_timer_handle_t stop_timer_ = nullptr;
    esp_timer_handle_t ramp_timer_ = nullptr;

    std::mutex motor_mutex_;  // guards the four speeds below and every motor_->Drive()
    int target_left_ = 0;
    int target_right_ = 0;
    int current_left_ = 0;
    int current_right_ = 0;

    std::atomic<bool> moving_{false};
    std::atomic<uint32_t> dance_generation_{0};
    std::atomic<bool> dancing_{false};
    int default_speed_ = 70;

    // Speed at 100 % on a flat floor. The defaults are a guess: the web page's calibration replaces them (saved in NVS)
    float cm_per_s_full_ = 35.0f;
    float deg_per_s_full_ = 300.0f;
    std::atomic<int> test_kind_{-1};
    std::atomic<int> test_speed_{0};
    std::atomic<int> test_ms_{0};
};

#endif  // ROBOT_CONTROLLER_H
