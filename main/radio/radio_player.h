#ifndef RADIO_PLAYER_H
#define RADIO_PLAYER_H

// Internet radio / music-URL player.
//
// Plays an HLS playlist (.m3u8 with AAC / MP3 / MPEG-TS segments, which is what the VOV
// channels use) or a plain HTTP(S) AAC / MP3 stream. Two tasks:
//   fetch  : downloads playlist + segments into a PSRAM stream buffer (absorbs Wi-Fi jitter)
//   decode : decodes with esp_audio_simple_dec, downmixes to mono, resamples to the codec rate
//            and hands PCM to AudioService::PushPcmToPlaybackQueue()
//
// Playback only runs while the device is idle: a wake word, a button press or a server
// message moves the state machine away from Idle and the player stops by itself.

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include <esp_audio_simple_dec.h>
#include "radio_channels.h"
#include <esp_ae_rate_cvt.h>

// One thing the player can try: a radio station or a track.
struct RadioSource {
    std::string name;
    std::string url;
};

class RadioPlayer {
public:
    static RadioPlayer& GetInstance() {
        static RadioPlayer instance;
        return instance;
    }

    static const std::vector<RadioChannel>& Channels() { return RadioChannels::List(); }
    // Finds a channel from an id ("vov1") or from free text the voice assistant produced
    // ("VOV giao thong Ha Noi", "đài Tây Nguyên", "tiếng Anh"). nullptr when nothing matches.
    static const RadioChannel* Find(const std::string& query) { return RadioChannels::Find(query); }

    // `source` is a channel id from Channels() or an http(s) URL; `title` overrides the name shown
    // on the display. Returns false (and fills `error`) when the source is unknown.
    bool Play(const std::string& source, std::string* error = nullptr, const std::string& title = "");

    // Plays the first entry that works. When an entry cannot be opened, never produces sound
    // within ~25 s, or (finite == false, a live station) ends by itself, the next entry is tried
    // automatically; Next() skips to the next entry on request. It does NOT move on when the
    // device is interrupted (wake word, button) or stays busy. finite == true: a track that
    // plays to its end is a normal finish, not a failure.
    bool PlayList(std::vector<RadioSource> sources, bool finite, std::string* error = nullptr);
    bool Next(std::string* error = nullptr);
    size_t Remaining() const;  // entries after the current one
    void Stop();
    bool IsPlaying() const;
    std::string NowPlaying() const;

    // Observers for displays / LEDs. Both run on player or caller tasks: keep them short.
    //  pcm: mono samples at the codec output rate, called for every decoded frame
    //  state: (title, playing) when audible playback starts and when it ends
    using PcmObserver = std::function<void(const int16_t* samples, size_t count, int sample_rate)>;
    using StateObserver = std::function<void(const std::string& title, bool playing)>;
    void SetPcmObserver(PcmObserver observer) { pcm_observer_ = std::move(observer); }
    void SetStateObserver(StateObserver observer) { state_observer_ = std::move(observer); }

private:
    RadioPlayer() = default;
    RadioPlayer(const RadioPlayer&) = delete;
    RadioPlayer& operator=(const RadioPlayer&) = delete;

    enum StreamType { kTypeUnknown, kTypeAac, kTypeMp3, kTypeTs };

    static void SuperEntry(void* arg);
    void Supervise();
    bool StartStream(const std::string& url, const std::string& name, std::string* error);
    void StopStream();
    bool StreamEnded() const;

    static void FetchEntry(void* arg);
    static void DecodeEntry(void* arg);
    void FetchTask();
    void DecodeTask();

    bool FetchHls(const std::string& playlist_url);
    bool FetchDirect(const std::string& url);
    bool DownloadInto(const std::string& url, bool detect_type_from_body);
    bool WriteBytes(const uint8_t* data, size_t len);
    bool ShouldStop() const;
    void Cleanup();

    bool OpenDecoder(StreamType type);
    void EmitPcm(const uint8_t* data, size_t bytes);

    PcmObserver pcm_observer_;
    StateObserver state_observer_;

    mutable std::mutex mutex_;
    std::string source_url_;
    std::string name_;

    // Supervisor (list playback, automatic station / track switching)
    std::recursive_mutex control_mutex_;
    std::vector<RadioSource> queue_;
    size_t queue_index_ = 0;
    bool finite_ = false;
    std::atomic<bool> super_running_{false};
    std::atomic<bool> super_cancel_{false};
    std::atomic<bool> skip_requested_{false};
    std::atomic<bool> started_audio_{false};   // sound reached the speaker
    std::atomic<bool> ready_to_play_{false};   // stream opened and pre-buffered
    std::atomic<bool> interrupted_{false};     // device left Idle on its own: stop, do not switch
    std::atomic<bool> busy_abort_{false};      // device never became Idle: stop, do not switch
    SemaphoreHandle_t super_done_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> fetch_done_{false};
    std::atomic<int> stream_type_{kTypeUnknown};
    EventGroupHandle_t done_event_ = nullptr;
    StreamBufferHandle_t stream_ = nullptr;

    esp_audio_simple_dec_handle_t decoder_ = nullptr;
    esp_ae_rate_cvt_handle_t resampler_ = nullptr;
    int resampler_src_rate_ = 0;
    int pcm_rate_ = 0;
    int pcm_channels_ = 0;
    std::vector<uint8_t> out_buffer_;
};

#endif  // RADIO_PLAYER_H
