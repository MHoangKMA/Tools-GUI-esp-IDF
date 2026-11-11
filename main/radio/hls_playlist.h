#ifndef HLS_PLAYLIST_H
#define HLS_PLAYLIST_H

// Reads an HLS (.m3u8) playlist. Plain C++, no ESP-IDF headers, so it can be unit-tested on a PC.

#include <string>
#include <vector>

struct HlsPlaylist {
    bool valid = false;            // starts with #EXTM3U
    bool is_master = false;        // lists variants (#EXT-X-STREAM-INF) instead of segments
    std::string variant_url;       // first variant of a master playlist, absolute
    long long media_sequence = 0;  // sequence number of the first segment
    int target_duration = 6;       // seconds
    bool ended = false;            // #EXT-X-ENDLIST: not a live stream
    bool has_fmp4 = false;         // fragmented MP4 segments / init section: not supported
    std::vector<std::string> segments;  // absolute URLs, oldest first
};

// `base_url` is the playlist's own address, used to resolve relative entries.
HlsPlaylist ParseHlsPlaylist(const std::string& text, const std::string& base_url);

// Resolves `ref` (absolute, //host/path, /path or relative) against `base`.
std::string ResolveUrl(const std::string& base, const std::string& ref);

#endif  // HLS_PLAYLIST_H
