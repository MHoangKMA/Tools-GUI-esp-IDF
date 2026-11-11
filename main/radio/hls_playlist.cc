#include "hls_playlist.h"

#include <cstdlib>

namespace {

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

std::string WithoutQuery(const std::string& url) { return url.substr(0, url.find_first_of("?#")); }

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::vector<std::string> NonEmptyLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = Trim(text.substr(pos, end - pos));
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
        pos = end + 1;
    }
    return lines;
}

}  // namespace

std::string ResolveUrl(const std::string& base, const std::string& ref) {
    if (StartsWith(ref, "http://") || StartsWith(ref, "https://")) {
        return ref;
    }
    auto scheme_end = base.find("://");
    if (scheme_end == std::string::npos) {
        return ref;
    }
    if (StartsWith(ref, "//")) {
        return base.substr(0, scheme_end + 1) + ref;
    }
    auto path_start = base.find('/', scheme_end + 3);
    std::string origin = path_start == std::string::npos ? base : base.substr(0, path_start);
    if (!ref.empty() && ref[0] == '/') {
        return origin + ref;
    }
    std::string path = path_start == std::string::npos ? "/" : WithoutQuery(base.substr(path_start));
    path = path.substr(0, path.rfind('/') + 1);
    return origin + path + ref;
}

HlsPlaylist ParseHlsPlaylist(const std::string& text, const std::string& base_url) {
    HlsPlaylist playlist;
    auto lines = NonEmptyLines(text);
    if (lines.empty() || !StartsWith(lines[0], "#EXTM3U")) {
        return playlist;
    }
    playlist.valid = true;

    // Master playlist: the first variant is the one we follow
    for (size_t i = 0; i < lines.size(); i++) {
        if (StartsWith(lines[i], "#EXT-X-STREAM-INF")) {
            playlist.is_master = true;
            for (size_t j = i + 1; j < lines.size(); j++) {
                if (lines[j][0] != '#') {
                    playlist.variant_url = ResolveUrl(base_url, lines[j]);
                    break;
                }
            }
            return playlist;
        }
    }

    for (const auto& line : lines) {
        if (StartsWith(line, "#EXT-X-MEDIA-SEQUENCE:")) {
            playlist.media_sequence = atoll(line.c_str() + 22);
        } else if (StartsWith(line, "#EXT-X-TARGETDURATION:")) {
            int duration = atoi(line.c_str() + 22);
            playlist.target_duration = duration > 0 ? duration : 6;
        } else if (StartsWith(line, "#EXT-X-ENDLIST")) {
            playlist.ended = true;
        } else if (StartsWith(line, "#EXT-X-MAP")) {
            playlist.has_fmp4 = true;
        } else if (line[0] != '#') {
            if (line.find(".m4s") != std::string::npos || line.find(".mp4") != std::string::npos) {
                playlist.has_fmp4 = true;
            }
            playlist.segments.push_back(ResolveUrl(base_url, line));
        }
    }
    return playlist;
}
