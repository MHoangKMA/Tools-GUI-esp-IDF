#include "server_profiles.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "sdkconfig.h"

#define TAG "ServerProfiles"
#define NVS_NAMESPACE "wifi"
#define KEY_ACTIVE "ota_url"
#define KEY_PROFILES "srv_profiles"

namespace {

constexpr size_t kMaxNameLength = 40;
constexpr size_t kMaxUrlLength = 200;

std::string ReadString(const char* key) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return "";
    }
    size_t length = 0;
    std::string value;
    if (nvs_get_str(handle, key, nullptr, &length) == ESP_OK && length > 1) {
        value.resize(length);
        if (nvs_get_str(handle, key, value.data(), &length) == ESP_OK) {
            value.resize(length - 1);  // drop the terminating zero
        } else {
            value.clear();
        }
    }
    nvs_close(handle);
    return value;
}

bool WriteString(const char* key, const std::string& value) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_str(handle, key, value.c_str());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

bool ValidUrl(const std::string& url) {
    return url.size() >= 10 && url.size() <= kMaxUrlLength &&
           (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0);
}

void Fail(std::string* error, const char* message) {
    if (error) {
        *error = message;
    }
}

bool Save(const std::vector<ServerProfile>& profiles) {
    cJSON* array = cJSON_CreateArray();
    for (const auto& profile : profiles) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", profile.name.c_str());
        cJSON_AddStringToObject(item, "url", profile.url.c_str());
        cJSON_AddItemToArray(array, item);
    }
    char* text = cJSON_PrintUnformatted(array);
    bool ok = text != nullptr && WriteString(KEY_PROFILES, text);
    cJSON_free(text);
    cJSON_Delete(array);
    return ok;
}

}  // namespace

std::string ServerProfiles::DefaultUrl() {
#ifdef CONFIG_OTA_URL
    return CONFIG_OTA_URL;
#else
    return "";
#endif
}

std::string ServerProfiles::ActiveUrl() {
    std::string url = ReadString(KEY_ACTIVE);
    return url.empty() ? DefaultUrl() : url;
}

std::vector<ServerProfile> ServerProfiles::List() {
    std::vector<ServerProfile> profiles;
    std::string text = ReadString(KEY_PROFILES);
    if (text.empty()) {
        return profiles;
    }
    cJSON* array = cJSON_Parse(text.c_str());
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, array) {
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* url = cJSON_GetObjectItem(item, "url");
        if (cJSON_IsString(name) && cJSON_IsString(url) && ValidUrl(url->valuestring)) {
            profiles.push_back({name->valuestring, url->valuestring});
        }
    }
    cJSON_Delete(array);
    return profiles;
}

bool ServerProfiles::Add(const std::string& name, const std::string& url, std::string* error) {
    if (name.empty() || name.size() > kMaxNameLength) {
        Fail(error, "name must be 1-40 characters");
        return false;
    }
    if (!ValidUrl(url)) {
        Fail(error, "url must start with http:// or https://");
        return false;
    }
    auto profiles = List();
    for (auto& profile : profiles) {
        if (profile.url == url) {  // same server: just rename it
            profile.name = name;
            return Save(profiles);
        }
    }
    if (url == DefaultUrl()) {
        Fail(error, "that is the built-in default server");
        return false;
    }
    if (profiles.size() >= kMaxProfiles) {
        Fail(error, "too many profiles, delete one first");
        return false;
    }
    profiles.push_back({name, url});
    return Save(profiles);
}

bool ServerProfiles::Remove(size_t index) {
    auto profiles = List();
    if (index >= profiles.size()) {
        return false;
    }
    const std::string removed = profiles[index].url;
    profiles.erase(profiles.begin() + index);
    if (!Save(profiles)) {
        return false;
    }
    if (ActiveUrl() == removed) {  // the active server was deleted: fall back to the default
        WriteString(KEY_ACTIVE, DefaultUrl());
    }
    return true;
}

bool ServerProfiles::Select(const std::string& url, std::string* error) {
    bool known = url == DefaultUrl();
    for (const auto& profile : List()) {
        known = known || profile.url == url;
    }
    if (!known) {
        Fail(error, "unknown server: add it first");
        return false;
    }
    if (!WriteString(KEY_ACTIVE, url)) {
        Fail(error, "cannot save");
        return false;
    }
    ESP_LOGI(TAG, "Active server: %s", url.c_str());
    return true;
}

bool ServerProfiles::VerifyKey(const std::string& key) {
    static int failures = 0;
    static int64_t blocked_until_us = 0;
    if (esp_timer_get_time() < blocked_until_us) {
        return false;
    }
    std::string expected;
    nvs_handle_t handle;
    if (nvs_open("robot", NVS_READONLY, &handle) == ESP_OK) {
        size_t length = 0;
        if (nvs_get_str(handle, "ota_token", nullptr, &length) == ESP_OK && length > 1) {
            expected.resize(length);
            if (nvs_get_str(handle, "ota_token", expected.data(), &length) == ESP_OK) {
                expected.resize(length - 1);
            } else {
                expected.clear();
            }
        }
        nvs_close(handle);
    }
    bool ok = !expected.empty() && key.size() == expected.size();
    if (ok) {
        uint8_t diff = 0;  // constant-time compare
        for (size_t i = 0; i < expected.size(); i++) {
            diff |= static_cast<uint8_t>(key[i] ^ expected[i]);
        }
        ok = diff == 0;
    }
    if (ok) {
        failures = 0;
    } else if (++failures >= 5) {
        failures = 0;
        blocked_until_us = esp_timer_get_time() + 60LL * 1000000;
    }
    return ok;
}

std::string ServerProfiles::ToJson() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "default_url", DefaultUrl().c_str());
    cJSON_AddStringToObject(root, "active_url", ActiveUrl().c_str());
    cJSON* array = cJSON_AddArrayToObject(root, "profiles");
    for (const auto& profile : List()) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", profile.name.c_str());
        cJSON_AddStringToObject(item, "url", profile.url.c_str());
        cJSON_AddItemToArray(array, item);
    }
    char* text = cJSON_PrintUnformatted(root);
    std::string json = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(root);
    return json;
}
