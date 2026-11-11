#include "radio_channels.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>

const std::vector<RadioChannel>& RadioChannels::List() {
    // Public stream addresses of the VOV channels (checked against the radio-browser.info
    // directory). Operators change them from time to time: pass any other http(s) URL to
    // Play() when a preset stops working.
    static const std::vector<RadioChannel> channels = {
        {"vov1", "VOV1", "https://str.vov.gov.vn/vovlive/vov1vov5Vietnamese.sdp_aac/playlist.m3u8"},
        {"vov2", "VOV2", "https://audio-lss.vov.vn/live/vov2.m3u8"},
        {"vov3", "VOV3", "https://audio-lss.vov.vn/live/vov3.m3u8"},
        {"vovgt_hn", "VOV Giao thong Ha Noi", "https://play.vovgiaothong.vn/live/gthn/playlist.m3u8"},
        {"vovgt_hcm", "VOV Giao thong TP.HCM", "https://play.vovgiaothong.vn/live/gthcm/playlist.m3u8"},
        // Added from the Vietnam fork of xiaozhi-esp32 (xiaozhi-esp32_vietnam). These URLs were
        // NOT re-checked: if one does not play, replace it or pass another URL to Play().
        {"vov5", "VOV5 Doi ngoai", "https://stream.vovmedia.vn/vov5"},
        {"vov5_english", "VOV5 English 24/7", "https://stream.vovmedia.vn/vov247"},
        {"vov_mekong", "VOV Mekong FM", "https://stream.vovmedia.vn/vovmekong"},
        {"vov4_mientrung", "VOV4 Mien Trung", "https://stream.vovmedia.vn/vov4mt"},
        {"vov4_taybac", "VOV4 Tay Bac", "https://stream.vovmedia.vn/vov4tb"},
        {"vov4_dongbac", "VOV4 Dong Bac", "https://stream.vovmedia.vn/vov4db"},
        {"vov4_taynguyen", "VOV4 Tay Nguyen", "https://stream.vovmedia.vn/vov4tn"},
        {"vov4_dbscl", "VOV4 DBSCL", "https://stream.vovmedia.vn/vov4dbscl"},
        {"vov4_hcm", "VOV4 TP.HCM", "https://stream.vovmedia.vn/vov4hcm"},
    };
    return channels;
}

const RadioChannel* RadioChannels::Find(const std::string& query) {
    std::string q;
    for (unsigned char ch : query) {
        q += (ch < 128) ? static_cast<char>(std::tolower(ch)) : static_cast<char>(ch);
    }
    auto by_id = [](const char* id) -> const RadioChannel* {
        for (const auto& channel : List()) {
            if (strcmp(channel.id, id) == 0) {
                return &channel;
            }
        }
        return nullptr;
    };
    auto has = [&q](std::initializer_list<const char*> words) {
        for (const char* word : words) {
            if (q.find(word) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    if (const RadioChannel* exact = by_id(q.c_str())) {
        return exact;
    }
    // Keyword matching for what a Vietnamese speech recogniser typically returns. Specific
    // channels first, generic ones last. Both accented and plain spellings are listed.
    if (has({"english", "tieng anh", "tiếng anh"})) return by_id("vov5_english");
    if (has({"giao thong", "giao thông", "vovgt"})) {
        return has({"hcm", "sai gon", "sài gòn", "ho chi minh", "hồ chí minh"}) ? by_id("vovgt_hcm")
                                                                                : by_id("vovgt_hn");
    }
    if (has({"tay nguyen", "tây nguyên"})) return by_id("vov4_taynguyen");
    if (has({"tay bac", "tây bắc"})) return by_id("vov4_taybac");
    if (has({"dong bac", "đông bắc"})) return by_id("vov4_dongbac");
    if (has({"mien trung", "miền trung"})) return by_id("vov4_mientrung");
    if (has({"dbscl", "cuu long", "cửu long"})) return by_id("vov4_dbscl");
    if (has({"mekong", "mê kông", "me kong", "mê-kông"})) return by_id("vov_mekong");
    if (has({"vov5", "vov 5", "doi ngoai", "đối ngoại"})) return by_id("vov5");
    if (has({"vov3", "vov 3", "am nhac", "âm nhạc", "giai tri", "giải trí"})) return by_id("vov3");
    if (has({"vov2", "vov 2", "van hoa", "văn hóa", "giao duc", "giáo dục"})) return by_id("vov2");
    if (has({"vov1", "vov 1", "thoi su", "thời sự", "tin tuc", "tin tức", "vov"})) return by_id("vov1");
    // Last resort: partial match on the display name
    for (const auto& channel : List()) {
        std::string name;
        for (const char* p = channel.name; *p; p++) {
            name += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
        }
        if (name.find(q) != std::string::npos) {
            return &channel;
        }
    }
    return nullptr;
}

