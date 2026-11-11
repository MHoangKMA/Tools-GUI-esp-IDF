#include "robot_selftest.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <server_profiles.h>
#include <wifi_manager.h>

#include <algorithm>
#include <cmath>

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "settings.h"

#define TAG "SelfTest"

namespace {
struct Note {
    int frequency_hz;
    int duration_ms;
};
const std::vector<Note> kStartTune = {{523, 90}};
const std::vector<Note> kOkTune = {{880, 70}, {1320, 90}};
const std::vector<Note> kUnknownTune = {{660, 110}};
const std::vector<Note> kFailTune = {{262, 180}, {196, 280}};
const std::vector<Note> kDoneTune = {{523, 100}, {659, 100}, {784, 100}, {1047, 260}};

void PlayTune(const std::vector<Note>& tune) {
    for (const auto& note : tune) {
        StartupSelfTest::Beep(note.frequency_hz, note.duration_ms);
    }
}

bool Flag(const char* key, bool fallback) {
    Settings settings("robot", false);
    return settings.GetBool(key, fallback);
}

// Name of the server profile ("agent") the robot talks to
std::string ActiveAgentName() {
    const std::string active = ServerProfiles::ActiveUrl();
    for (const auto& profile : ServerProfiles::List()) {
        if (profile.url == active) {
            return profile.name;
        }
    }
    return active == ServerProfiles::DefaultUrl() ? "mac dinh" : "tuy chinh";
}
}  // namespace

StartupSelfTest& StartupSelfTest::GetInstance() {
    static StartupSelfTest instance;
    return instance;
}

void StartupSelfTest::AddStep(const std::string& name, StepFn fn) { steps_.push_back({name, std::move(fn)}); }

std::string StartupSelfTest::Report() const {
    std::lock_guard<std::mutex> lock(report_mutex_);
    return report_;
}

void StartupSelfTest::Beep(int frequency_hz, int duration_ms) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr || codec->output_sample_rate() <= 0) {
        return;
    }
    const int rate = codec->output_sample_rate();
    const int samples = rate * duration_ms / 1000;
    const int fade = std::max(1, rate * 6 / 1000);  // 6 ms in and out: no click
    std::vector<int16_t> pcm(samples);
    for (int i = 0; i < samples; i++) {
        float envelope = 1.0f;
        if (i < fade) {
            envelope = static_cast<float>(i) / fade;
        } else if (i > samples - fade) {
            envelope = static_cast<float>(samples - i) / fade;
        }
        pcm[i] = static_cast<int16_t>(9000.0f * envelope * std::sin(2.0f * 3.14159265f * frequency_hz * i / rate));
    }
    Application::GetInstance().GetAudioService().PushPcmToPlaybackQueue(std::move(pcm), true);
    vTaskDelay(pdMS_TO_TICKS(duration_ms + 30));
}

void StartupSelfTest::Start() {
    if (Flag("selftest", true)) {
        Launch(true);
    }
}

bool StartupSelfTest::RunAgain() { return Launch(false); }

bool StartupSelfTest::Launch(bool wait_for_idle) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return false;
    }
    wait_for_idle_ = wait_for_idle;
    if (xTaskCreate(TaskEntry, "selftest", 6144, this, 3, nullptr) != pdPASS) {
        running_ = false;
        return false;
    }
    return true;
}

void StartupSelfTest::TaskEntry(void* arg) {
    auto* self = static_cast<StartupSelfTest*>(arg);
    self->Run();
    self->running_ = false;
    vTaskDelete(nullptr);
}

void StartupSelfTest::Show(const std::string& text, int duration_ms) {
    Application::GetInstance().Schedule([text, duration_ms]() {
        auto display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->ShowNotification(text, duration_ms);
        }
    });
}

void StartupSelfTest::Present(const std::string& name, const StepResult& result) {
    const char* tag = result.unknown ? "?  " : (result.ok ? "OK " : "LOI");
    std::string line = std::string(tag) + " " + name + (result.detail.empty() ? "" : ": " + result.detail);
    if (result.ok) {
        ESP_LOGI(TAG, "%s", line.c_str());
    } else {
        ESP_LOGW(TAG, "%s", line.c_str());
    }
    {
        std::lock_guard<std::mutex> lock(report_mutex_);
        report_ += line + "\n";
    }
    Show(line, 2500);
    PlayTune(result.unknown ? kUnknownTune : (result.ok ? kOkTune : kFailTune));
    vTaskDelay(pdMS_TO_TICKS(250));
}

void StartupSelfTest::Run() {
    auto& app = Application::GetInstance();
    if (wait_for_idle_) {
        // Boot: wait for the robot to be ready (Wi-Fi, activation). If it never is, test what there is.
        for (int i = 0; i < 900 && app.GetDeviceState() != kDeviceStateIdle; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(1500));  // let the "ready" sound and the notifications finish
    }
    {
        std::lock_guard<std::mutex> lock(report_mutex_);
        report_.clear();
    }
    ESP_LOGI(TAG, "---- self test ----");
    Show("Kiem tra linh kien...", 2000);
    PlayTune(kStartTune);
    vTaskDelay(pdMS_TO_TICKS(500));

    int ok = 0;
    int unknown = 0;
    std::vector<std::string> failed;
    bool server_ok = false;
    auto step = [&](const std::string& name, const StepFn& fn) {
        StepResult result = fn();
        Present(name, result);
        if (result.unknown) {
            unknown++;
        } else if (result.ok) {
            ok++;
        } else {
            failed.push_back(name);
        }
        return result;
    };

    // The beep that announces the step is the speaker test itself
    step("Loa", []() { return StepResult{true, false, "nghe tieng bip"}; });

    step("Wi-Fi", []() {
        auto& wifi = WifiManager::GetInstance();
        if (!wifi.IsConnected()) {
            return StepResult{false, false, "chua ket noi"};
        }
        return StepResult{true, false, wifi.GetSsid() + " " + wifi.GetIpAddress()};
    });

    const std::string agent = ActiveAgentName();
    server_ok = step("May chu / agent", [&]() {
        if (app.GetDeviceState() != kDeviceStateIdle) {
            return StepResult{false, false, "chua san sang"};
        }
        return StepResult{true, false, agent};
    }).ok;

    for (const auto& s : steps_) {
        step(s.name, s.fn);
    }

    // Microphone: needs a voice, so it can only ever be "heard" or "not confirmed"
    step("Micro", [&]() {
        auto& audio = app.GetAudioService();
        if (!audio.IsWakeWordRunning() && !audio.IsAudioProcessorRunning()) {
            return StepResult{true, true, "bo qua (khong co bo loc giong noi)"};
        }
        Show("Hay noi mot cau...", 4000);
        for (int i = 0; i < 50; i++) {
            if (audio.IsVoiceDetected()) {
                return StepResult{true, false, "nghe thay giong noi"};
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return StepResult{true, true, "chua nghe thay giong"};
    });

    // Summary
    std::string summary = std::to_string(ok) + " OK";
    if (unknown > 0) {
        summary += ", " + std::to_string(unknown) + " chua xac nhan";
    }
    if (!failed.empty()) {
        std::string names;
        for (const auto& name : failed) {
            names += (names.empty() ? "" : ", ") + name;
        }
        summary += ", LOI: " + names;
        {
            std::lock_guard<std::mutex> lock(report_mutex_);
            report_ += "Ket qua: " + summary + "\n";
        }
        ESP_LOGW(TAG, "Self test: %s", summary.c_str());
        Show("Loi: " + names, 8000);
        PlayTune(kFailTune);
        Application::GetInstance().Schedule([]() { Board::GetInstance().GetDisplay()->SetEmotion("sad"); });
    } else {
        {
            std::lock_guard<std::mutex> lock(report_mutex_);
            report_ += "Ket qua: " + summary + " - khoi dong thanh cong\n";
        }
        ESP_LOGI(TAG, "Self test: %s", summary.c_str());
        Show("Khoi dong thanh cong! Agent: " + agent, 6000);
        PlayTune(kDoneTune);
        app.Schedule([]() { Board::GetInstance().GetDisplay()->SetEmotion("happy"); });
        if (server_ok && Flag("selftest_voice", true)) {
            // The robot has no voice of its own: the server speaks the sentence in the agent's voice
            const std::string sentence =
                "Hãy chỉ nói đúng một câu, không thêm gì khác: Tôi đã khởi động thành công, agent " + agent +
                " đã sẵn sàng.";
            vTaskDelay(pdMS_TO_TICKS(600));
            app.Schedule([sentence]() {
                if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                    Application::GetInstance().WakeWordInvoke(sentence);
                }
            });
        }
    }
}
