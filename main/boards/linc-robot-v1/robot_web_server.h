#ifndef ROBOT_WEB_SERVER_H
#define ROBOT_WEB_SERVER_H

#include <string>

#include <esp_event.h>
#include <esp_http_server.h>

// Control + status page of the robot, served on port 8080 of the station IP:
//   http://<robot-ip>:8080/
// D-pad driving, dance, OLED faces, radio, device status and "update firmware now".
//
// Security model: the page is for the local network only. State-changing calls are POST
// with a custom X-Robot header (browsers refuse to send that cross-site without a CORS
// preflight, which this server never answers).
//   * Driving, radio, faces: an optional token (NVS "robot" / "web_token") in X-Token.
//   * Server / agent choice, music source, firmware check and upload: ALWAYS the admin key
//     (NVS "robot" / "ota_token"), generated on first boot and shown on the robot's own
//     screen only. Five wrong keys block admin calls for a minute.
// The server stops while the Wi-Fi setup hotspot is up, because that network is open.
class RobotWebServer {
public:
    static RobotWebServer& GetInstance();

    // Returns the admin key, creating it on first use. Call it at boot so the song OLED can show
    // it before any web page is opened. The key protects every sensitive action: changing the
    // server (agent), the music source, and firmware update / upload.
    static std::string EnsureAdminKey();

    // Registers the Wi-Fi event handlers; the server starts when an IP address is obtained.
    void Initialize();

    bool running() const { return server_ != nullptr; }

private:
    RobotWebServer() = default;

    void Start();
    void Stop();
    static void OnEvent(void* arg, esp_event_base_t base, int32_t id, void* data);

    httpd_handle_t server_ = nullptr;
};

#endif  // ROBOT_WEB_SERVER_H
