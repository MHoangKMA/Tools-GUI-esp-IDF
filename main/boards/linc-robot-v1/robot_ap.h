#ifndef ROBOT_AP_H
#define ROBOT_AP_H

#include <esp_event.h>

#include <mutex>
#include <string>

// The robot's own Wi-Fi hotspot, running next to the station connection (APSTA).
//
// While the robot is connected to the router it also broadcasts "Linc-Robot-XXXX" (WPA2). A phone that joins it
// opens http://192.168.4.1:8080/ to drive the robot with no router in between. The phone gets no Internet from
// the robot (no routing): it is only a remote control. The station keeps working, so voice chat, radio and music
// are not interrupted.
//
// The hotspot starts when the station gets an IP and goes away when the Wi-Fi setup portal takes over
// (WifiConfigurationAp reuses / replaces the same AP interface).
//
// Settings (NVS namespace "robot"): ap_on (default 1), ap_pass (default 12345678, 8-63 characters).
class RobotAp {
public:
    static RobotAp& GetInstance();

    void Initialize();           // registers for "station got an IP"
    void Apply();                // settings changed: restart the hotspot (or remove it when disabled)

    std::string Ssid() const;
    std::string Password() const;
    bool Enabled() const;
    bool Active() const;         // the hotspot of this class is up
    std::string Url() const;     // http://<ap ip>:8080/

    // true during the few seconds after Start(): the web server must not mistake our AP_START for the setup portal
    static bool JustStarted();
    // true when the AP interface currently carries our SSID
    static bool OwnsAp();

private:
    RobotAp() = default;
    static void OnEvent(void* arg, esp_event_base_t base, int32_t id, void* data);
    void Start();
    void Stop();

    mutable std::mutex mutex_;
    std::string ap_ip_ = "192.168.4.1";
};

#endif  // ROBOT_AP_H
