#include "ota_upload.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"

#define TAG "OtaUpload"

namespace ota_upload {
namespace {

constexpr size_t kChunkSize = 4096;
constexpr int kMaxReceiveTimeouts = 10;

esp_err_t Reply(httpd_req_t* req, const char* status, const char* error) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    char body[96];
    snprintf(body, sizeof(body), "{\"success\":false,\"error\":\"%s\"}", error);
    httpd_resp_sendstr(req, body);
    return ESP_FAIL;
}

void ShowMessage(const std::string& message) {
    Application::GetInstance().Schedule([message]() {
        Board::GetInstance().GetDisplay()->SetChatMessage("system", message.c_str());
    });
}

}  // namespace

esp_err_t Handle(httpd_req_t* req) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();

    // Only while idle: the device must not be talking, listening or already upgrading
    if (app.GetDeviceState() != kDeviceStateIdle) {
        return Reply(req, "409 Conflict", "busy");
    }
    const esp_partition_t* update = esp_ota_get_next_update_partition(nullptr);
    if (update == nullptr) {
        return Reply(req, "500 Internal Server Error", "no_partition");
    }
    if (req->content_len == 0 || req->content_len > update->size) {
        return Reply(req, "400 Bad Request", "invalid_length");
    }
    std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[kChunkSize]);
    if (!buffer) {
        return Reply(req, "500 Internal Server Error", "out_of_memory");
    }
    if (!app.SetDeviceState(kDeviceStateUpgrading)) {
        return Reply(req, "409 Conflict", "busy");
    }
    ESP_LOGI(TAG, "Receiving %u bytes into %s", static_cast<unsigned>(req->content_len), update->label);

    app.Schedule([&app]() {
        app.Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    });
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    app.GetAudioService().Stop();

    // Restores normal operation after any failure
    auto fail = [&](const char* status, const char* code) {
        ESP_LOGE(TAG, "Upload failed: %s", code);
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        app.GetAudioService().Start();
        app.SetDeviceState(kDeviceStateIdle);
        app.Schedule([&app]() {
            app.Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "cancel", Lang::Sounds::OGG_EXCLAMATION);
        });
        return Reply(req, status, code);
    };

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return fail("500 Internal Server Error", "ota_begin_failed");
    }

    size_t received = 0;
    int timeouts = 0;
    int last_percent = -1;
    while (received < req->content_len) {
        size_t want = std::min(kChunkSize, req->content_len - received);
        int n = httpd_req_recv(req, reinterpret_cast<char*>(buffer.get()), want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > kMaxReceiveTimeouts) {
                esp_ota_abort(handle);
                return fail("408 Request Timeout", "receive_timeout");
            }
            continue;
        }
        if (n <= 0) {
            esp_ota_abort(handle);
            return fail("400 Bad Request", "receive_failed");
        }
        timeouts = 0;
        err = esp_ota_write(handle, buffer.get(), n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            return fail("500 Internal Server Error", "write_failed");
        }
        received += n;
        int percent = static_cast<int>(received * 100 / req->content_len);
        if (percent / 5 != last_percent / 5) {
            last_percent = percent;
            ShowMessage(std::to_string(percent) + "%");
        }
    }

    // Verifies the image structure and checksum
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return fail("400 Bad Request", err == ESP_ERR_OTA_VALIDATE_FAILED ? "invalid_image" : "ota_end_failed");
    }
    // Refuse an image built for another project (a valid but wrong .bin would brick the robot)
    esp_app_desc_t new_desc;
    if (esp_ota_get_partition_description(update, &new_desc) != ESP_OK ||
        strncmp(new_desc.project_name, esp_app_get_description()->project_name, sizeof(new_desc.project_name)) != 0) {
        return fail("400 Bad Request", "wrong_project");
    }
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return fail("500 Internal Server Error", "set_boot_failed");
    }

    ESP_LOGI(TAG, "Firmware %s accepted, rebooting", new_desc.version);
    ShowMessage("Upgrade successful, rebooting...");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"success\":true}");
    app.Schedule([&app]() { app.Reboot(); });
    return ESP_OK;
}

}  // namespace ota_upload
