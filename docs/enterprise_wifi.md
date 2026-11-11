# Enterprise / school WiFi and captive portals

`local_components/esp-wifi-connect` is a local copy of `78/esp-wifi-connect` 3.3.1, wired in through `override_path` in `main/idf_component.yml`. It is not under `components/`, so IDF does not load it twice. Changes are marked `[SANDBOX]`.

## What it does

- **WPA2/WPA3-Enterprise (802.1X, PEAP/TTLS-MSCHAPv2):** a network whose scan record is enterprise uses the saved account through the EAP client. Server certificates are not validated (the ESP-IDF default).
- **Setup page:** checkbox "Network needs an account" plus a Username field. It is ticked automatically for 🏢 networks. Pressing Connect on an enterprise network performs the real 802.1X authentication, so a wrong account or password fails on the page.
- **Captive portal (public / hotel / school web login):** after DHCP the station probes `connectivitycheck.gstatic.com/generate_204`. A redirect or HTML page means a portal. The form is filled with the saved account, or the terms are accepted for click-through portals, then the probe repeats. `Connected` is announced only after that. Plain HTML forms only: no JavaScript, OTP/SMS, CAPTCHA or vendor-app portals. Portal credentials are first verified at the real station connection, not in config mode (the setup AP has no station IP).
- NVS `wifi/portal_check` = 0 disables the probe. `SsidItem.username` is stored as `username`, `username1`, ... `SsidManager::AddSsid` has a trailing default `username` parameter, so existing callers are unchanged.
- `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT` is `y` in `sdkconfig.defaults` (upstream has it `n` to save memory), which costs flash and RAM.

## How this copy was produced (read this before relying on it)

The copy was **restored from the text of an earlier session, without fetching it again**:

| Part | State |
|---|---|
| `wifi_station.*`, `wifi_manager.*`, `wifi_configuration_ap.*`, `ssid_manager.cc`, `CMakeLists.txt`, `idf_component.yml` | Recovered verbatim (line counts match the originals), then the changes above applied |
| `wifi_enterprise.*`, `wifi_portal.*` | New files, restored as written |
| `include/ssid_manager.h` | Reproduced from a quick view of the original plus the new `username` field |
| `dns_server.{h,cc}` | **Re-written** from the interface used by `wifi_configuration_ap.cc` (answers every A query with the hotspot address). Not byte-identical to upstream |
| `assets/wifi_configuration.html`, `wifi_configuration_done.html` | **Re-written.** The original page (about 40 languages, its own styling) was never available. The new page has the same endpoints and features but only English, Vietnamese and Simplified Chinese; other languages fall back to English |
| `README`, `LICENSE`, screenshots | Not restored. The upstream project is MIT licensed; keep its license notice if you redistribute |

Merging a newer upstream release later will need a manual compare of `dns_server.*` and the two HTML pages.

## Not compiled

Check `esp_wifi_sta_enterprise_enable/disable` and `esp_eap_client_*` against ESP-IDF 6.0.1 on the first build, and the new DNS server (socket / task) and web pages on a real device.
