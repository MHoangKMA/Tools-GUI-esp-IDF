#include "radio_directory.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <cJSON.h>
#include <esp_log.h>

#include "board.h"
#include "url_util.h"
#include "http.h"

#define TAG "RadioDirectory"

namespace {

constexpr int kHttpTimeoutMs = 10000;
constexpr int kMaxLimit = 20;

// Mirrors of the directory. The first one answers today; the others are tried when it does
// not (they were not checked from here, replace them if the project changes its hosts).
const char* const kHosts[] = {"de1.api.radio-browser.info", "de2.api.radio-browser.info",
                              "fi1.api.radio-browser.info"};

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// RadioPlayer decodes AAC, MP3 and HLS (TS / AAC / MP3 segments) only
bool SupportedCodec(const std::string& codec, bool is_hls) {
    if (is_hls) {
        return true;
    }
    std::string c = Lower(codec);
    return c == "mp3" || c == "aac" || c == "aac+" || c == "unknown" || c.empty();
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

}  // namespace

bool RadioDirectory::Search(const std::string& name, const std::string& country, const std::string& tag,
                            int limit, std::vector<RadioSource>* stations, std::string* error) {
    auto fail = [error](const std::string& message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    limit = std::clamp(limit, 1, kMaxLimit);
    std::string query = "hidebroken=true&order=clickcount&reverse=true&limit=" + std::to_string(limit * 2);
    if (!name.empty()) {
        query += "&name=" + url::Encode(name) + "&name_exact=false";
    }
    if (!country.empty()) {
        query += "&countrycode=" + url::Encode(country);
    }
    if (!tag.empty()) {
        query += "&tag=" + url::Encode(tag) + "&tag_exact=false";
    }

    std::string body;
    for (const char* host : kHosts) {
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
        if (!http) {
            continue;
        }
        http->SetTimeout(kHttpTimeoutMs);
        http->SetHeader("User-Agent", "xiaozhi-esp32-radio/1.0");
        http->SetHeader("Accept", "application/json");
        auto opened = http->Open("GET", std::string("https://") + host + "/json/stations/search?" + query);
        if (!opened) {
            ESP_LOGW(TAG, "%s: %s", host, opened.error().ToString().c_str());
            continue;
        }
        auto status = http->GetStatusCode();
        if (status && *status == 200) {
            body = http->ReadAll();
            http->Close();
            if (!body.empty()) {
                break;
            }
        } else {
            http->Close();
        }
    }
    if (body.empty()) {
        return fail("Cannot reach the radio directory");
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return fail("Radio directory sent an invalid answer");
    }

    std::vector<RadioSource> found;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        cJSON* resolved = cJSON_GetObjectItem(item, "url_resolved");
        cJSON* plain = cJSON_GetObjectItem(item, "url");
        cJSON* station_name = cJSON_GetObjectItem(item, "name");
        cJSON* codec = cJSON_GetObjectItem(item, "codec");
        cJSON* hls = cJSON_GetObjectItem(item, "hls");
        cJSON* ssl_error = cJSON_GetObjectItem(item, "ssl_error");

        std::string url = cJSON_IsString(resolved) && resolved->valuestring[0] ? resolved->valuestring
                          : cJSON_IsString(plain) ? plain->valuestring : "";
        std::string title = cJSON_IsString(station_name) ? Trim(station_name->valuestring) : "";
        if (url.empty() || title.empty() || (cJSON_IsNumber(ssl_error) && ssl_error->valueint != 0)) {
            continue;
        }
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
            continue;
        }
        const bool is_hls = cJSON_IsNumber(hls) && hls->valueint != 0;
        if (!SupportedCodec(cJSON_IsString(codec) ? codec->valuestring : "", is_hls)) {
            continue;
        }
        if (std::any_of(found.begin(), found.end(), [&url](const RadioSource& s) { return s.url == url; })) {
            continue;
        }
        found.push_back({title, url});
        if (static_cast<int>(found.size()) >= limit) {
            break;
        }
    }
    cJSON_Delete(root);

    if (found.empty()) {
        return fail("No playable station found");
    }
    if (stations) {
        *stations = std::move(found);
    }
    return true;
}
