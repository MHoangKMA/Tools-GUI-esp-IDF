#ifndef ROBOT_SELFTEST_H
#define ROBOT_SELFTEST_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Start-up self test: after the robot has booted and reached "idle", it goes through its parts one by one.
// For each part it shows a short line on the screen, plays a beep (rising = OK, low = failed, single = could not
// be checked) and logs the result. When everything is fine it plays a jingle, shows "started successfully" with the
// name of the active server / agent and, if enabled, asks the server to say it aloud.
//
// Generic parts are checked here (speaker, Wi-Fi, server / agent, microphone). The board adds its own parts
// (screens, lights, wheels, hotspot...) with AddStep().
//
// What can and cannot be verified: a step can only tell that a part answered (bus device found, driver started,
// connection up). Whether the wheels turned or the screen shows the right picture is for the person watching.
//
// Settings (NVS namespace "robot"): selftest (default 1) runs at every boot, selftest_motor (default 1) wiggles the
// wheels in place for a fraction of a second, selftest_voice (default 1) asks the server to say the greeting.
class StartupSelfTest {
public:
    struct StepResult {
        bool ok = true;
        bool unknown = false;  // could not be checked: neither a success nor a failure
        std::string detail;
    };
    using StepFn = std::function<StepResult()>;

    static StartupSelfTest& GetInstance();

    // Board specific parts, run in the order added (before Start()). Names: plain ASCII, they go to small screens.
    void AddStep(const std::string& name, StepFn fn);

    void Start();             // boot: waits for idle, then runs once (if the setting allows)
    bool RunAgain();          // web page: false when a test is already running
    bool Running() const { return running_.load(); }
    std::string Report() const;

    // Short beeps through the normal playback path (for the steps that need to be heard)
    static void Beep(int frequency_hz, int duration_ms);

private:
    StartupSelfTest() = default;
    bool Launch(bool wait_for_idle);
    static void TaskEntry(void* arg);
    void Run();
    void Present(const std::string& name, const StepResult& result);
    void Show(const std::string& text, int duration_ms);

    struct Step {
        std::string name;
        StepFn fn;
    };
    std::vector<Step> steps_;
    std::atomic<bool> running_{false};
    bool wait_for_idle_ = true;
    mutable std::mutex report_mutex_;
    std::string report_;
};

#endif  // ROBOT_SELFTEST_H
