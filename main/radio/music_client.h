#ifndef MUSIC_CLIENT_H
#define MUSIC_CLIENT_H

// Resolves "song name (+ artist)" to a streamable audio URL through a music search server,
// then RadioPlayer plays that URL. Ported from the Vietnam fork of xiaozhi-esp32
// (features/music/esp32_music.cc), same HTTP contract:
//
//   GET <server>/stream_pcm?song=<name>&artist=<name>
//     headers: X-MAC-Address, X-Chip-ID, X-Timestamp, X-Dynamic-Key
//     200 JSON: { "title", "artist", "audio_url", "lyric_url", "duration" }
//
// A second, free source is Jamendo (jamendo.com: independent music under Creative Commons
// licences, free API key from developer.jamendo.com). Enter the key as "client id" and songs
// are searched there; both sources can be set, the music server is tried first.
//
// The server address is NOT built in. Set it in NVS namespace "wifi", key "music_url" (the same
// key the fork uses), e.g. from the robot web page. Whoever runs that server receives the
// device MAC address and the song names you ask for, so choose a server you trust.

#include <string>
#include <vector>

struct MusicTrack {
    std::string title;
    std::string artist;
    std::string audio_url;
    std::string lyric_url;  // not used yet
    int duration_s = 0;
};

class MusicClient {
public:
    static std::string ServerUrl();
    static bool SetServerUrl(const std::string& url);  // false when it is not a valid http(s) URL

    static std::string JamendoClientId();
    static bool SetJamendoClientId(const std::string& id);  // letters / digits only

    // Candidates for a song, best first: the music server's answer, then Jamendo results.
    static bool Search(const std::string& song, const std::string& artist, std::vector<MusicTrack>* tracks,
                       std::string* error);

    // Search() then RadioPlayer::PlayList(): when a track cannot be played the next candidate
    // is tried. `title` receives the first candidate's title.
    static bool PlaySong(const std::string& song, const std::string& artist, std::string* title,
                         std::string* error);

    // Blocking (a few seconds). `error` receives a short reason on failure.
    static bool Resolve(const std::string& song, const std::string& artist, MusicTrack* track,
                        std::string* error);
};

#endif  // MUSIC_CLIENT_H
