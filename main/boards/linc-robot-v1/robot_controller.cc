#include "robot_controller.h"

#include <algorithm>
#include <cJSON.h>
#include <esp_log.h>
#include <wifi_manager.h>

#include <cmath>

#include "application.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#include "music_client.h"
#include "radio_channels.h"
#include "radio_directory.h"
#include "radio_player.h"
#include "settings.h"
#include "signal_pattern.h"
#include "turn_signals.h"

#define TAG "RobotController"

namespace {
constexpr uint64_t kRampPeriodUs = 20 * 1000;
constexpr int kRampStepPercent = 12;
constexpr int kTestSpeed = 70;  // calibration runs
constexpr int kTestForwardMs = 2000;
constexpr int kTestTurnMs = 1000;
constexpr int kMinExactSpeed = 20;  // below this the wheels often do not turn at all

int Approach(int current, int target) {
    return current + std::clamp(target - current, -kRampStepPercent, kRampStepPercent);
}
}  // namespace

RobotController& RobotController::GetInstance() {
    static RobotController instance;
    return instance;
}

void RobotController::Initialize(Drv8833Motor* motor) {
    motor_ = motor;
    esp_timer_create_args_t args = {};
    args.callback = StopTimerCallback;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "robot_stop";
    ESP_ERROR_CHECK(esp_timer_create(&args, &stop_timer_));

    args.callback = RampTimerCallback;
    args.name = "robot_ramp";
    ESP_ERROR_CHECK(esp_timer_create(&args, &ramp_timer_));

    Settings settings("robot", false);
    int cm_x10 = settings.GetInt("cal_cm_x10", 0);
    int deg = settings.GetInt("cal_deg", 0);
    if (cm_x10 > 0) {
        cm_per_s_full_ = cm_x10 / 10.0f;
    }
    if (deg > 0) {
        deg_per_s_full_ = static_cast<float>(deg);
    }
    ESP_LOGI(TAG, "Calibration: %.1f cm/s and %.0f deg/s at full speed%s", cm_per_s_full_, deg_per_s_full_,
             cm_x10 > 0 || deg > 0 ? "" : " (defaults, not calibrated)");
}

void RobotController::set_default_speed(int speed) { default_speed_ = std::clamp(speed, 0, 100); }

// ---------- motion ----------

void RobotController::SetTarget(int left, int right) {
    {
        std::lock_guard<std::mutex> lock(motor_mutex_);
        target_left_ = std::clamp(left, -100, 100);
        target_right_ = std::clamp(right, -100, 100);
    }
    if (left != 0 || right != 0) {
        moving_ = true;
        TurnSignals::GetInstance().SetMovement(MovementPattern(left, right));
    }
    // Starting an already running periodic timer only returns ESP_ERR_INVALID_STATE
    esp_timer_start_periodic(ramp_timer_, kRampPeriodUs);
}

void RobotController::RampStep() {
    bool stopped = false;
    {
        std::lock_guard<std::mutex> lock(motor_mutex_);
        current_left_ = Approach(current_left_, target_left_);
        current_right_ = Approach(current_right_, target_right_);
        motor_->Drive(current_left_, current_right_);
        if (current_left_ == target_left_ && current_right_ == target_right_) {
            esp_timer_stop(ramp_timer_);  // arrived: nothing left to ramp
            stopped = target_left_ == 0 && target_right_ == 0;
        }
    }
    if (stopped) {
        moving_ = false;
        TurnSignals::GetInstance().SetMovement(SignalPattern::kOff);
    }
}

void RobotController::RampTimerCallback(void* arg) { static_cast<RobotController*>(arg)->RampStep(); }

// The move time is over: slow down to a stop
void RobotController::StopTimerCallback(void* arg) {
    auto* self = static_cast<RobotController*>(arg);
    if (!self->dancing_.load()) {
        self->SetTarget(0, 0);
    }
}

bool RobotController::Move(const std::string& direction, int speed, int duration_ms, std::string* error) {
    return MoveLimited(direction, speed, duration_ms, kMaxDurationMs, error);
}

bool RobotController::MoveLimited(const std::string& direction, int speed, int duration_ms, int max_ms,
                                  std::string* error) {
    int left = 0;
    int right = 0;
    if (direction == "forward") {
        left = right = speed;
    } else if (direction == "backward") {
        left = right = -speed;
    } else if (direction == "left") {  // spin counter-clockwise
        left = -speed;
        right = speed;
    } else if (direction == "right") {  // spin clockwise
        left = speed;
        right = -speed;
    } else {
        if (error) {
            *error = "direction must be forward, backward, left or right";
        }
        return false;
    }
    // A manual move cancels a running dance
    dance_generation_++;
    dancing_ = false;

    duration_ms = std::clamp(duration_ms, 50, max_ms);
    esp_timer_stop(stop_timer_);
    SetTarget(left, right);
    esp_timer_start_once(stop_timer_, static_cast<uint64_t>(duration_ms) * 1000);
    return true;
}

// ---------- exact moves (open loop) ----------
//
// There are no wheel encoders, so a distance is a time: cm / (cm_per_s_full * speed / 100). The speed ramp is
// symmetric (what is lost while accelerating is gained while slowing down), so the formula needs no correction.
// Accuracy is a few cm / degrees at best: it depends on battery charge, floor and load. Calibrate on the real floor.

bool RobotController::MoveDistance(const std::string& direction, int cm, int speed, std::string* error) {
    if (direction != "forward" && direction != "backward") {
        if (error) {
            *error = "a distance needs direction forward or backward";
        }
        return false;
    }
    speed = std::clamp(speed, kMinExactSpeed, 100);
    cm = std::max(cm, 1);
    float velocity = cm_per_s_full_ * speed / 100.0f;  // cm per second
    int ms = static_cast<int>(std::lround(cm * 1000.0f / velocity));
    if (ms > kMaxDistanceMs) {
        if (error) {
            *error = "too far for one command: at speed " + std::to_string(speed) + " the limit is " +
                     std::to_string(static_cast<int>(velocity * kMaxDistanceMs / 1000)) + " cm";
        }
        return false;
    }
    ESP_LOGI(TAG, "Move %d cm %s at %d%% = %d ms", cm, direction.c_str(), speed, ms);
    return MoveLimited(direction, speed, ms, kMaxDistanceMs, error);
}

bool RobotController::Turn(const std::string& direction, int degrees, int speed, std::string* error) {
    if (direction != "left" && direction != "right") {
        if (error) {
            *error = "an angle needs direction left or right";
        }
        return false;
    }
    speed = std::clamp(speed, kMinExactSpeed, 100);
    degrees = std::max(degrees, 1);
    float rate = deg_per_s_full_ * speed / 100.0f;  // degrees per second
    int ms = static_cast<int>(std::lround(degrees * 1000.0f / rate));
    if (ms > kMaxDistanceMs) {
        if (error) {
            *error = "turn too long for one command: use a higher speed or a smaller angle";
        }
        return false;
    }
    ESP_LOGI(TAG, "Turn %d deg %s at %d%% = %d ms", degrees, direction.c_str(), speed, ms);
    return MoveLimited(direction, speed, ms, kMaxDistanceMs, error);
}

bool RobotController::RunTest(int kind, std::string* error) {
    if (kind != 0 && kind != 1) {
        if (error) {
            *error = "kind must be 0 (straight) or 1 (spin)";
        }
        return false;
    }
    int ms = kind == 0 ? kTestForwardMs : kTestTurnMs;
    test_kind_ = -1;  // a failed start must not leave a stale test to calibrate from
    if (!MoveLimited(kind == 0 ? "forward" : "right", kTestSpeed, ms, kMaxDistanceMs, error)) {
        return false;
    }
    test_speed_ = kTestSpeed;
    test_ms_ = ms;
    test_kind_ = kind;
    return true;
}

bool RobotController::SaveCalibration(int kind, float measured, std::string* error) {
    if (test_kind_.load() != kind) {
        if (error) {
            *error = "run the test of this kind first, then measure what the robot did";
        }
        return false;
    }
    if (!(measured > 0.0f)) {
        if (error) {
            *error = "the measured value must be above zero";
        }
        return false;
    }
    // measured = rate_at_full * (speed / 100) * seconds
    float rate = measured / ((test_ms_.load() / 1000.0f) * (test_speed_.load() / 100.0f));
    Settings settings("robot", true);
    if (kind == 0) {
        cm_per_s_full_ = std::clamp(rate, 2.0f, 300.0f);
        settings.SetInt("cal_cm_x10", static_cast<int>(std::lround(cm_per_s_full_ * 10.0f)));
    } else {
        deg_per_s_full_ = std::clamp(rate, 30.0f, 2000.0f);
        settings.SetInt("cal_deg", static_cast<int>(std::lround(deg_per_s_full_)));
    }
    test_kind_ = -1;
    ESP_LOGI(TAG, "Calibration saved: %.1f cm/s, %.0f deg/s at full speed", cm_per_s_full_, deg_per_s_full_);
    return true;
}

void RobotController::Stop() {
    dance_generation_++;
    dancing_ = false;
    esp_timer_stop(stop_timer_);
    esp_timer_stop(ramp_timer_);
    {
        std::lock_guard<std::mutex> lock(motor_mutex_);
        target_left_ = target_right_ = current_left_ = current_right_ = 0;
        motor_->Stop();
    }
    moving_ = false;
    TurnSignals::GetInstance().SetMovement(SignalPattern::kOff);
}

void RobotController::Dance(int speed) {
    if (speed >= 0) {
        set_default_speed(speed);
    }
    dance_generation_++;
    uint32_t generation = dance_generation_.load();
    esp_timer_stop(stop_timer_);
    dancing_ = true;
    // The task receives the generation through the pointer-sized argument
    xTaskCreate(DanceTask, "robot_dance", 4096,
                reinterpret_cast<void*>(static_cast<uintptr_t>(generation)), 2, nullptr);
}

void RobotController::DanceTask(void* arg) {
    auto& self = GetInstance();
    const uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    auto display = Board::GetInstance().GetDisplay();

    struct Step {
        int left;
        int right;
        int ms;
        const char* emotion;
    };
    const int s = self.default_speed_;
    const Step steps[] = {
        {s, -s, 500, "happy"},   {-s, s, 500, "laughing"}, {s, -s, 500, "funny"},
        {-s, s, 500, "happy"},   {s, s, 400, "laughing"},  {-s, -s, 400, "funny"},
        {s, -s, 1200, "happy"},  {0, 0, 300, "neutral"},
    };
    for (const auto& step : steps) {
        if (self.dance_generation_.load() != generation) {
            vTaskDelete(nullptr);  // stopped or superseded: whoever did that owns the motors now
        }
        display->SetEmotion(step.emotion);
        self.SetTarget(step.left, step.right);
        vTaskDelay(pdMS_TO_TICKS(step.ms));
    }
    if (self.dance_generation_.load() == generation) {
        self.SetTarget(0, 0);
        self.dancing_ = false;
    }
    vTaskDelete(nullptr);
}

// ---------- voice tools ----------
//
// Few, broad tools: the cloud side accepts a bounded number of tools and drops the rest quietly.

void RobotController::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.robot.move",
                "Drive the wheeled robot, then stop automatically. direction: forward, backward, left or right "
                "(left/right spin in place). speed: 0-100. Three ways to say how far: "
                "(1) distance_cm for an exact distance, only with forward/backward (for 'go forward 5 cm' use "
                "forward and distance_cm 5); (2) degrees for an exact turn, only with left/right (for 'turn right "
                "90 degrees' use right and degrees 90); (3) otherwise duration_ms 100-5000. Leave distance_cm and "
                "degrees at 0 to use the duration.",
                PropertyList({Property("direction", kPropertyTypeString, std::string("forward")),
                              Property("speed", kPropertyTypeInteger, 70, 0, 100),
                              Property("duration_ms", kPropertyTypeInteger, 1000, 100, kMaxDurationMs),
                              Property("distance_cm", kPropertyTypeInteger, 0, 0, 300),
                              Property("degrees", kPropertyTypeInteger, 0, 0, 360)}),
                [this](const PropertyList& properties) -> ToolResult {
                    std::string error;
                    const std::string direction = properties["direction"].value<std::string>();
                    const int speed = properties["speed"].value<int>();
                    const int distance = properties["distance_cm"].value<int>();
                    const int degrees = properties["degrees"].value<int>();
                    bool ok;
                    if (distance > 0) {
                        ok = MoveDistance(direction, distance, speed, &error);
                    } else if (degrees > 0) {
                        ok = Turn(direction, degrees, speed, &error);
                    } else {
                        ok = Move(direction, speed, properties["duration_ms"].value<int>(), &error);
                    }
                    if (!ok) {
                        return std::unexpected(error);
                    }
                    return ReturnValue(true);
                });

    mcp.AddTool("self.robot.stop", "Stop the wheels immediately and cancel a dance.", PropertyList(),
                [this](const PropertyList& properties) -> ReturnValue {
                    Stop();
                    return true;
                });

    mcp.AddTool("self.robot.dance",
                "Make the robot dance for a few seconds (spins and wiggles with happy faces). "
                "speed: 0-100.",
                PropertyList({Property("speed", kPropertyTypeInteger, 70, 0, 100)}),
                [this](const PropertyList& properties) -> ReturnValue {
                    Dance(properties["speed"].value<int>());
                    return true;
                });

    mcp.AddTool("self.robot.set_emotion",
                "Change the face (two eyes) shown on the screen. emotion: " + std::string(
                    "neutral, relaxed, cool, bored, confident, happy, laughing, funny, silly, loving, "
                    "embarrassed, winking, kissy, delicious, excited, sad, crying, worried, scared, angry, "
                    "surprised, shocked, sleepy, sleeping, thinking, confused, dizzy"),
                PropertyList({Property("emotion", kPropertyTypeString, std::string("happy"))}),
                [](const PropertyList& properties) -> ReturnValue {
                    std::string emotion = properties["emotion"].value<std::string>();
                    Application::GetInstance().Schedule([emotion]() {
                        Board::GetInstance().GetDisplay()->SetEmotion(emotion.c_str());
                    });
                    return true;
                });

    mcp.AddTool("self.robot.lights",
                "Blink the robot's two signal lights (left and right turn signals). mode: left, right, both "
                "(hazard), alternate (left and right take turns), fast, slow, off. duration_ms: 500-30000, how "
                "long to blink. The lights also blink by themselves while the robot drives: the left light "
                "when it turns left, the right light when it turns right, both when it goes forward or "
                "backward; and they flash with some faces (angry, happy, surprised ...).",
                PropertyList({Property("mode", kPropertyTypeString, std::string("both")),
                              Property("duration_ms", kPropertyTypeInteger, 5000, 500, 30000)}),
                [](const PropertyList& properties) -> ToolResult {
                    SignalPattern pattern;
                    if (!ParseSignalMode(properties["mode"].value<std::string>(), &pattern)) {
                        return std::unexpected(
                            std::string("mode must be left, right, both, alternate, fast, slow or off"));
                    }
                    TurnSignals::GetInstance().SetManual(pattern, properties["duration_ms"].value<int>());
                    return ReturnValue(true);
                });

    mcp.AddTool("self.robot.get_status", "Get robot status: IP address, Wi-Fi, wheels and radio.", PropertyList(),
                [this](const PropertyList& properties) -> ReturnValue {
                    auto& wifi = WifiManager::GetInstance();
                    cJSON* root = cJSON_CreateObject();
                    cJSON_AddStringToObject(root, "ip", wifi.GetIpAddress().c_str());
                    cJSON_AddStringToObject(root, "ssid", wifi.GetSsid().c_str());
                    cJSON_AddNumberToObject(root, "rssi", wifi.GetRssi());
                    cJSON_AddBoolToObject(root, "moving", IsMoving());
                    cJSON_AddStringToObject(root, "lights", SignalName(TurnSignals::GetInstance().Current()));
                    cJSON_AddNumberToObject(root, "speed", default_speed());
                    cJSON_AddBoolToObject(root, "radio_playing", RadioPlayer::GetInstance().IsPlaying());
                    cJSON_AddStringToObject(root, "radio", RadioPlayer::GetInstance().NowPlaying().c_str());
                    char* text = cJSON_PrintUnformatted(root);
                    std::string result(text);
                    cJSON_free(text);
                    cJSON_Delete(root);
                    return result;
                });

    mcp.AddTool(
        "self.radio.play",
        "Play an internet radio station through the speaker. name: a built-in channel id (vov1, vov2, vov3, "
        "vovgt_hn, vovgt_hcm, vov5, vov5_english, vov_mekong, vov4_mientrung, vov4_taybac, vov4_dongbac, "
        "vov4_taynguyen, vov4_dbscl, vov4_hcm), a channel name as the user said it ('VOV giao thong Ha Noi', "
        "'dai Tay Nguyen', 'tieng Anh'), any other station name or kind of radio ('nhac vang', 'tin tuc'; "
        "searched in the free radio-browser.info directory, and if a station does not work the robot moves "
        "on to the next one by itself), or a full http(s) stream URL. country: two-letter code for the "
        "directory search, default VN. tag: genre for the directory search, optional. Playback starts after "
        "this conversation ends.",
        PropertyList({Property("name", kPropertyTypeString, std::string("vov1")),
                      Property("country", kPropertyTypeString, std::string("VN")),
                      Property("tag", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& properties) -> ToolResult {
            std::string name = properties["name"].value<std::string>();
            std::string error;
            auto& player = RadioPlayer::GetInstance();
            const bool is_url = name.rfind("http://", 0) == 0 || name.rfind("https://", 0) == 0;
            // 1. a URL or one of the built-in channels (id or spoken name)
            if (is_url || (!name.empty() && RadioChannels::Find(name) != nullptr)) {
                if (!player.Play(name, &error)) {
                    return std::unexpected(error);
                }
                return ReturnValue(std::string("Playing: ") + name);
            }
            // 2. the free station directory
            std::vector<RadioSource> stations;
            if (!RadioDirectory::Search(name, properties["country"].value<std::string>(),
                                        properties["tag"].value<std::string>(), 8, &stations, &error)) {
                return std::unexpected(error);
            }
            std::string first = stations.front().name;
            size_t others = stations.size() - 1;
            if (!player.PlayList(std::move(stations), false, &error)) {
                return std::unexpected(error);
            }
            return ReturnValue(std::string("Playing: ") + first + " (" + std::to_string(others) +
                               " more stations ready; use self.media.control next to change station)");
        });

    mcp.AddTool("self.music.play",
                "Play a song through the speaker. song: the song title; artist: the singer, optional. "
                "Searches free music (Jamendo and/or the music server set on the robot web page); if a track "
                "cannot be played the robot tries the next result by itself. Or give url: a direct http(s) link "
                "to an MP3 / AAC file or stream (for example from a search tool); song is then only the "
                "title shown on the screen. Playback starts after this conversation ends.",
                PropertyList({Property("song", kPropertyTypeString, std::string("")),
                              Property("artist", kPropertyTypeString, std::string("")),
                              Property("url", kPropertyTypeString, std::string(""))}),
                [](const PropertyList& properties) -> ToolResult {
                    std::string song = properties["song"].value<std::string>();
                    std::string artist = properties["artist"].value<std::string>();
                    std::string url = properties["url"].value<std::string>();
                    std::string error;
                    if (!url.empty()) {
                        if (!RadioPlayer::GetInstance().Play(url, &error, song)) {
                            return std::unexpected(error);
                        }
                        return ReturnValue(std::string("Playing: ") + (song.empty() ? url : song));
                    }
                    if (song.empty()) {
                        return std::unexpected(std::string("Give a song name or a url"));
                    }
                    std::string title;
                    if (!MusicClient::PlaySong(song, artist, &title, &error)) {
                        return std::unexpected(error);
                    }
                    return ReturnValue(std::string("Playing: ") + title);
                });

    mcp.AddTool("self.media.control",
                "Control the radio or music that is playing. action: 'next' skips to the next station or "
                "track of the list (use when the user says it is bad, does not work or asks to change it), "
                "'stop' stops playback.",
                PropertyList({Property("action", kPropertyTypeString, std::string("stop"))}),
                [](const PropertyList& properties) -> ToolResult {
                    std::string action = properties["action"].value<std::string>();
                    std::string error;
                    if (action == "next") {
                        if (!RadioPlayer::GetInstance().Next(&error)) {
                            return std::unexpected(error);
                        }
                    } else if (action == "stop") {
                        RadioPlayer::GetInstance().Stop();
                    } else {
                        return std::unexpected(std::string("action must be next or stop"));
                    }
                    return ReturnValue(true);
                });
}
