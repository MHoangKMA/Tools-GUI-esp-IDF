#ifndef _WIFI_ENTERPRISE_H_
#define _WIFI_ENTERPRISE_H_

// [SANDBOX] WPA2/WPA3-Enterprise (802.1X: PEAP / TTLS with MSCHAPv2) helpers shared by
// WifiStation and WifiConfigurationAp. Requires CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=y.

#include <string>

#include <esp_err.h>
#include <esp_wifi_types_generic.h>

// True when a scan record advertises 802.1X authentication.
bool WifiIsEnterpriseAuth(wifi_auth_mode_t authmode);

// Programs identity / username / password into the EAP client and switches the station
// to enterprise mode. Call after esp_wifi_set_config() and before esp_wifi_connect().
// The WPA-PSK password field of wifi_config_t must be left empty for these networks.
esp_err_t WifiEnterpriseEnable(const std::string& username, const std::string& password);

// Leaves enterprise mode and wipes the credentials from the EAP client. Safe to call when
// enterprise mode was never enabled.
void WifiEnterpriseDisable();

#endif // _WIFI_ENTERPRISE_H_
