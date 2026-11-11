#ifndef _SERVER_PROFILES_H_
#define _SERVER_PROFILES_H_

// [SANDBOX] Named server profiles: the robot can keep several servers (for example the cloud
// service and a self-hosted one) and switch between them from the setup page or the robot web
// page. A profile is the OTA / activation URL: the device asks it for the WebSocket or MQTT
// address, so selecting a profile selects the server (and the agent behind it).
//
// Cloud agents themselves (name, language, voice, role) are configured on the server's own
// dashboard; a device only chooses WHICH server to talk to.
//
// Storage: NVS namespace "wifi", key "ota_url" (the active one, same key the setup page's
// advanced tab and Ota::GetCheckVersionUrl() use) and key "srv_profiles" (JSON array).

#include <string>
#include <vector>

struct ServerProfile {
    std::string name;
    std::string url;
};

class ServerProfiles {
public:
    static constexpr size_t kMaxProfiles = 8;

    // URL compiled into the firmware (CONFIG_OTA_URL)
    static std::string DefaultUrl();
    // URL the device will use at the next activation
    static std::string ActiveUrl();
    // User profiles only (the built-in default is not stored)
    static std::vector<ServerProfile> List();

    static bool Add(const std::string& name, const std::string& url, std::string* error = nullptr);
    static bool Remove(size_t index);
    // Makes `url` the active server. It must be the default URL or one of the saved profiles.
    // Takes effect at the next activation (reboot, or leaving setup mode).
    static bool Select(const std::string& url, std::string* error = nullptr);

    // True when `key` equals the robot's admin key (NVS namespace "robot", key "ota_token", created
    // by the application at boot and shown only on the robot's own screen). False when no key has
    // been created yet. Five wrong keys block every check for a minute.
    static bool VerifyKey(const std::string& key);

    // {"default_url":..,"active_url":..,"profiles":[{"name":..,"url":..}]} for the web pages
    static std::string ToJson();
};

#endif  // _SERVER_PROFILES_H_
