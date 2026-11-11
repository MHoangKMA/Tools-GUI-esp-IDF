#include "robot_ap.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include <cstdio>
#include <cstring>

#include "application.h"
#include "board.h"
#include "display.h"
#include "settings.h"

#define TAG "RobotAp"

namespace {
constexpr char kApKey[] = "WIFI_AP_DEF";
constexpr char kStaKey[] = "WIFI_STA_DEF";
constexpr char kSsidPrefix[] = "Linc-Robot-";
constexpr char kDefaultPassword[] = "12345678";
constexpr int64_t kJustStartedUs = 4 * 1000 * 1000;
volatile int64_t g_started_us = -kJustStartedUs * 2;
}  // namespace

RobotAp& RobotAp::GetInstance() {
    static RobotAp instance;
    return instance;
}

bool RobotAp::JustStarted() { return esp_timer_get_time() - g_started_us < kJustStartedUs; }

bool RobotAp::OwnsAp() {
    wifi_config_t config = {};
    if (esp_wifi_get_config(WIFI_IF_AP, &config) != ESP_OK) {
        return false;
    }
    return strncmp(reinterpret_cast<const char*>(config.ap.ssid), kSsidPrefix, strlen(kSsidPrefix)) == 0;
}

void RobotAp::Initialize() {
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Cannot create event loop: %s", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &RobotAp::OnEvent, this));
}

void RobotAp::OnEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        static_cast<RobotAp*>(arg)->Start();
    }
}

bool RobotAp::Enabled() const {
    Settings settings("robot", false);
    return settings.GetBool("ap_on", true);
}

std::string RobotAp::Password() const {
    Settings settings("robot", false);
    std::string password = settings.GetString("ap_pass", kDefaultPassword);
    if (password.size() < 8 || password.size() > 63) {  // WPA2 limits: never start an AP that would fail
        password = kDefaultPassword;
    }
    return password;
}

std::string RobotAp::Ssid() const {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char text[24];
    snprintf(text, sizeof(text), "%s%02X%02X", kSsidPrefix, mac[4], mac[5]);
    return text;
}

bool RobotAp::Active() const { return esp_netif_get_handle_from_ifkey(kApKey) != nullptr && OwnsAp(); }

std::string RobotAp::Url() const { return "http://" + ap_ip_ + ":8080/"; }

void RobotAp::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Enabled()) {
        return;
    }
    if (esp_netif_get_handle_from_ifkey(kApKey) != nullptr) {
        return;  // already up (ours, after a reconnect) or the setup portal owns the interface
    }

    // Two networks on the same subnet cannot be routed: when the router also uses 192.168.4.x, move the hotspot
    bool clash = false;
    esp_netif_t* station = esp_netif_get_handle_from_ifkey(kStaKey);
    esp_netif_ip_info_t sta_info = {};
    if (station != nullptr && esp_netif_get_ip_info(station, &sta_info) == ESP_OK) {
        uint32_t ip = sta_info.ip.addr;  // network byte order: first octet is the lowest byte
        clash = (ip & 0xFF) == 192 && ((ip >> 8) & 0xFF) == 168 && ((ip >> 16) & 0xFF) == 4;
    }

    esp_netif_t* ap = esp_netif_create_default_wifi_ap();
    if (ap == nullptr) {
        ESP_LOGE(TAG, "Cannot create the hotspot interface");
        return;
    }
    if (clash) {
        esp_netif_ip_info_t info = {};
        info.ip.addr = ESP_IP4TOADDR(192, 168, 77, 1);
        info.gw.addr = ESP_IP4TOADDR(192, 168, 77, 1);
        info.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
        esp_netif_dhcps_stop(ap);
        esp_netif_set_ip_info(ap, &info);
        esp_netif_dhcps_start(ap);
        ap_ip_ = "192.168.77.1";
    } else {
        ap_ip_ = "192.168.4.1";
    }

    std::string ssid = Ssid();
    std::string password = Password();
    wifi_config_t config = {};
    strncpy(reinterpret_cast<char*>(config.ap.ssid), ssid.c_str(), sizeof(config.ap.ssid) - 1);
    config.ap.ssid_len = ssid.size();
    strncpy(reinterpret_cast<char*>(config.ap.password), password.c_str(), sizeof(config.ap.password) - 1);
    config.ap.max_connection = 3;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    // The channel follows the station connection (one radio): nothing to choose

    g_started_us = esp_timer_get_time();
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start the hotspot: %s", esp_err_to_name(err));
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_netif_destroy_default_wifi(ap);
        return;
    }
    ESP_LOGW(TAG, "Hotspot '%s' (password %s): open %s", ssid.c_str(), password.c_str(), Url().c_str());
    std::string note = "Wi-Fi: " + ssid + "  pass " + password;
    Application::GetInstance().Schedule([note]() { Board::GetInstance().GetDisplay()->ShowNotification(note, 20000); });
}

void RobotAp::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    esp_netif_t* ap = esp_netif_get_handle_from_ifkey(kApKey);
    if (ap == nullptr || !OwnsAp()) {
        return;  // not ours (setup portal): leave it alone
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_netif_destroy_default_wifi(ap);
}

void RobotAp::Apply() {
    Stop();
    esp_netif_t* station = esp_netif_get_handle_from_ifkey(kStaKey);
    esp_netif_ip_info_t info = {};
    if (station != nullptr && esp_netif_get_ip_info(station, &info) == ESP_OK && info.ip.addr != 0) {
        Start();  // only while the station is connected
    }
}
