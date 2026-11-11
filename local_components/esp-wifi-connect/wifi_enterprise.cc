#include "wifi_enterprise.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs.h>
#include "sdkconfig.h"

#if CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT
#include <esp_eap_client.h>
#endif

#define TAG "WifiEnterprise"

bool WifiIsEnterpriseAuth(wifi_auth_mode_t authmode) {
    switch (authmode) {
        case WIFI_AUTH_WPA2_ENTERPRISE:
        case WIFI_AUTH_WPA3_ENT_192:
        case WIFI_AUTH_WPA3_ENTERPRISE:
        case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
            return true;
        default:
            return false;
    }
}

#if CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT

esp_err_t WifiEnterpriseEnable(const std::string& username, const std::string& password) {
    if (username.empty()) {
        ESP_LOGE(TAG, "Enterprise network needs a username");
        return ESP_ERR_INVALID_ARG;
    }
    // Outer identity: the "anonymous identity" of NVS wifi/eap_identity (advanced tab of the setup
    // page) when set, because many schools require something like anonymous@school.edu.vn there;
    // otherwise the username, which works for most company PEAP/TTLS setups.
    std::string identity = username;
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        char stored[100] = {0};
        size_t size = sizeof(stored);
        if (nvs_get_str(nvs, "eap_identity", stored, &size) == ESP_OK && stored[0] != '\0') {
            identity = stored;
        }
        nvs_close(nvs);
    }
    esp_err_t err = esp_eap_client_set_identity(
        reinterpret_cast<const unsigned char*>(identity.data()), identity.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_identity failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_eap_client_set_username(
        reinterpret_cast<const unsigned char*>(username.data()), username.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_username failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_eap_client_set_password(
        reinterpret_cast<const unsigned char*>(password.data()), password.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_password failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_sta_enterprise_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enterprise_enable failed: %s", esp_err_to_name(err));
    }
    return err;
}

void WifiEnterpriseDisable() {
    esp_wifi_sta_enterprise_disable();
    esp_eap_client_clear_identity();
    esp_eap_client_clear_username();
    esp_eap_client_clear_password();
}

#else  // CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT

esp_err_t WifiEnterpriseEnable(const std::string& username, const std::string& password) {
    ESP_LOGE(TAG, "Enterprise WiFi is disabled: set CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=y");
    return ESP_ERR_NOT_SUPPORTED;
}

void WifiEnterpriseDisable() {}

#endif
