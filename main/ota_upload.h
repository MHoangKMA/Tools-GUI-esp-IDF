#ifndef OTA_UPLOAD_H
#define OTA_UPLOAD_H

// Firmware update by uploading a .bin file from a browser (the "OTA web server" feature of the
// Vietnam fork of xiaozhi-esp32, re-written). The request body is the raw image
// (Content-Type: application/octet-stream), which is streamed into the next OTA partition:
// no multipart parsing. After the image passes esp_ota_end() and the project-name check the
// boot partition is switched and the device reboots.
//
// This lets anyone who can call the endpoint replace the firmware, so the CALLER MUST
// AUTHENTICATE the request before calling Handle().

#include <esp_err.h>
#include <esp_http_server.h>

namespace ota_upload {

// Answers with JSON {"success":true} or {"success":false,"error":"<code>"}.
esp_err_t Handle(httpd_req_t* req);

}  // namespace ota_upload

#endif  // OTA_UPLOAD_H
