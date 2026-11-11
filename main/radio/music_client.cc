#include "music_client.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <psa/crypto.h>

#include "board.h"
#include "url_util.h"
#include "http.h"
#include "radio_player.h"
#include "settings.h"
#include "system_info.h"

#define TAG "MusicClient"

namespace {

constexpr int kHttpTimeoutMs = 10000;

// "<MAC>:<chip id>:<timestamp>:<secret>" hashed with SHA-256, first 16 bytes as upper-case hex.
// Same scheme as the fork. The default secret is the placeholder string shipped by the fork:
// override it with NVS "wifi"/"music_secret" when your server uses a real one.
std::string DynamicKey(const std::string& mac, const std::string& chip_id, int64_t timestamp) {
    Settings settings("wifi", false);
    std::string secret = settings.GetString("music_secret", "your-esp32-secret-key-2024");
    std::string data = mac + ":" + chip_id + ":" + std::to_string(timestamp) + ":" + secret;

    psa_crypto_init();
    uint8_t hash[32];
    size_t hash_length = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t*>(data.data()), data.size(), hash,
                         sizeof(hash), &hash_length) != PSA_SUCCESS) {
        return "";
    }
    std::string key;
    for (int i = 0; i < 16; i++) {
        char byte[3];
        snprintf(byte, sizeof(byte), "%02X", hash[i]);
        key += byte;
    }
    return key;
}

// base + path + "?" + query, with every query value percent-encoded (the fork's rule for
// paths that carry parameters).
std::string BuildUrlWithParams(const std::string& base, const std::string& path, const std::string& query) {
    std::string result = base + path + "?";
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        std::string param = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = param.find('=');
        result += eq == std::string::npos ? param : param.substr(0, eq) + "=" + url::Encode(param.substr(eq + 1));
        if (amp == std::string::npos) {
            break;
        }
        result += '&';
        pos = amp + 1;
    }
    return result;
}

// A path returned by the server ("/x?a=b") or an absolute URL, as a full URL.
std::string Absolute(const std::string& base, const std::string& path, bool strip_mp3_prefix) {
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0) {
        return path;
    }
    size_t question = path.find('?');
    if (question != std::string::npos && (!strip_mp3_prefix || path.find("/mp3/") == std::string::npos)) {
        return BuildUrlWithParams(base, path.substr(0, question), path.substr(question + 1));
    }
    std::string clean = path;
    if (strip_mp3_prefix) {
        // Older servers return /mp3/... paths that the stream endpoint serves without the prefix
        size_t mp3 = clean.find("/mp3/");
        if (mp3 != std::string::npos) {
            clean = clean.substr(mp3 + 4);
        }
    }
    return base + (clean.empty() || clean[0] == '/' ? "" : "/") + clean;
}

}  // namespace

std::string MusicClient::ServerUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("music_url");
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

bool MusicClient::SetServerUrl(const std::string& url) {
    if (url.size() > 200 || (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)) {
        return false;
    }
    Settings settings("wifi", true);
    settings.SetString("music_url", url);
    return true;
}

bool MusicClient::Resolve(const std::string& song, const std::string& artist, MusicTrack* track,
                          std::string* error) {
    auto fail = [error](const std::string& message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    const std::string base = ServerUrl();
    if (base.empty()) {
        return fail("Music server is not configured. Set it on the robot web page first.");
    }
    if (song.empty()) {
        return fail("No song name given");
    }

    const std::string url = base + "/stream_pcm?song=" + url::Encode(song) + "&artist=" + url::Encode(artist);
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!http) {
        return fail("Cannot create HTTP client");
    }
    const std::string mac = SystemInfo::GetMacAddress();
    std::string chip_id = mac;
    chip_id.erase(std::remove(chip_id.begin(), chip_id.end(), ':'), chip_id.end());
    const int64_t timestamp = esp_timer_get_time() / 1000000;

    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "application/json");
    http->SetHeader("X-MAC-Address", mac);
    http->SetHeader("X-Chip-ID", chip_id);
    http->SetHeader("X-Timestamp", std::to_string(timestamp));
    http->SetHeader("X-Dynamic-Key", DynamicKey(mac, chip_id, timestamp));

    ESP_LOGI(TAG, "Searching '%s' / '%s' on %s", song.c_str(), artist.c_str(), base.c_str());
    if (auto opened = http->Open("GET", url); !opened) {
        return fail("Cannot reach the music server: " + opened.error().ToString());
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        http->Close();
        return fail("Music server answered HTTP " + std::to_string(status ? *status : -1));
    }
    std::string body = http->ReadAll();
    http->Close();

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return fail("Music server sent an invalid answer");
    }
    auto text = [root](const char* key) -> std::string {
        cJSON* item = cJSON_GetObjectItem(root, key);
        return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
    };
    MusicTrack result;
    result.title = text("title");
    result.artist = text("artist");
    std::string audio = text("audio_url");
    std::string lyric = text("lyric_url");
    cJSON* duration = cJSON_GetObjectItem(root, "duration");
    result.duration_s = cJSON_IsNumber(duration) ? static_cast<int>(duration->valuedouble) : 0;
    cJSON_Delete(root);

    if (audio.empty()) {
        return fail("Song not found: " + song);
    }
    result.audio_url = Absolute(base, audio, true);
    result.lyric_url = lyric.empty() ? "" : Absolute(base, lyric, false);
    if (result.title.empty()) {
        result.title = song;
    }
    if (track) {
        *track = std::move(result);
    }
    return true;
}

std::string MusicClient::JamendoClientId() {
    Settings settings("wifi", false);
    return settings.GetString("jamendo_id");
}

bool MusicClient::SetJamendoClientId(const std::string& id) {
    if (id.size() > 40 || !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c); })) {
        return false;
    }
    Settings settings("wifi", true);
    if (id.empty()) {
        settings.EraseKey("jamendo_id");
    } else {
        settings.SetString("jamendo_id", id);
    }
    return true;
}

namespace {

// Jamendo API v3: GET /v3.0/tracks/ returns {"results":[{"name","artist_name","audio",...}]}
// where "audio" is a direct MP3 stream link.
bool SearchJamendo(const std::string& client_id, const std::string& song, const std::string& artist,
                   std::vector<MusicTrack>* out, std::string* error) {
    std::string text = artist.empty() ? song : song + " " + artist;
    std::string url = "https://api.jamendo.com/v3.0/tracks/?client_id=" + client_id +
                      "&format=json&limit=6&audioformat=mp31&order=relevance&search=" + url::Encode(text);
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!http) {
        if (error) *error = "Cannot create HTTP client";
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "application/json");
    if (auto opened = http->Open("GET", url); !opened) {
        if (error) *error = "Cannot reach Jamendo: " + opened.error().ToString();
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        http->Close();
        if (error) *error = "Jamendo answered HTTP " + std::to_string(status ? *status : -1);
        return false;
    }
    std::string body = http->ReadAll();
    http->Close();

    cJSON* root = cJSON_Parse(body.c_str());
    cJSON* results = root ? cJSON_GetObjectItem(root, "results") : nullptr;
    if (!cJSON_IsArray(results)) {
        cJSON_Delete(root);
        if (error) *error = "Jamendo sent an invalid answer";
        return false;
    }
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, results) {
        cJSON* audio = cJSON_GetObjectItem(item, "audio");
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* artist_name = cJSON_GetObjectItem(item, "artist_name");
        cJSON* duration = cJSON_GetObjectItem(item, "duration");
        if (!cJSON_IsString(audio) || audio->valuestring[0] == '\0') {
            continue;
        }
        MusicTrack track;
        track.audio_url = audio->valuestring;
        track.title = cJSON_IsString(name) ? name->valuestring : song;
        track.artist = cJSON_IsString(artist_name) ? artist_name->valuestring : "";
        track.duration_s = cJSON_IsNumber(duration) ? static_cast<int>(duration->valuedouble) : 0;
        out->push_back(std::move(track));
    }
    cJSON_Delete(root);
    return true;
}

}  // namespace

bool MusicClient::Search(const std::string& song, const std::string& artist, std::vector<MusicTrack>* tracks,
                         std::string* error) {
    std::vector<MusicTrack> found;
    std::string last_error;
    const bool has_server = !ServerUrl().empty();
    const std::string jamendo = JamendoClientId();
    if (!has_server && jamendo.empty()) {
        if (error) {
            *error = "No music source is set. Enter a Jamendo client id (free) or a music server on the robot web page.";
        }
        return false;
    }
    if (has_server) {
        MusicTrack track;
        if (Resolve(song, artist, &track, &last_error)) {
            found.push_back(std::move(track));
        }
    }
    if (!jamendo.empty()) {
        std::string jamendo_error;
        if (!SearchJamendo(jamendo, song, artist, &found, &jamendo_error) && last_error.empty()) {
            last_error = jamendo_error;
        }
    }
    if (found.empty()) {
        if (error) {
            *error = last_error.empty() ? "No song found for: " + song : last_error;
        }
        return false;
    }
    if (tracks) {
        *tracks = std::move(found);
    }
    return true;
}

bool MusicClient::PlaySong(const std::string& song, const std::string& artist, std::string* title,
                           std::string* error) {
    std::vector<MusicTrack> tracks;
    if (!Search(song, artist, &tracks, error)) {
        return false;
    }
    std::vector<RadioSource> sources;
    for (const auto& track : tracks) {
        sources.push_back({track.artist.empty() ? track.title : track.title + " - " + track.artist, track.audio_url});
    }
    if (title) {
        *title = sources.front().name;
    }
    // finite: a track that plays to its end is finished, only a track that fails is replaced
    return RadioPlayer::GetInstance().PlayList(std::move(sources), true, error);
}
