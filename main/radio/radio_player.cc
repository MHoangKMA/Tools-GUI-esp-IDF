#include "radio_player.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>

#include <esp_audio_dec_default.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/idf_additions.h>

#include "application.h"
#include "board.h"
#include "display.h"
#include "hls_playlist.h"
#include "http.h"

#define TAG "RadioPlayer"

namespace {
constexpr size_t kStreamBufferSize = 96 * 1024;
constexpr size_t kStreamBufferFallbackSize = 32 * 1024;
constexpr size_t kPrebufferBytes = 24 * 1024;
constexpr size_t kHttpChunk = 1460;
constexpr int kHttpTimeoutMs = 8000;
constexpr int kMaxPlaylistFailures = 5;
constexpr int kMaxDirectReconnects = 3;
constexpr int kWaitIdleTimeoutMs = 90 * 1000;
constexpr int kListeningGraceMs = 2500;
constexpr EventBits_t kFetchDoneBit = BIT0;
constexpr EventBits_t kDecodeDoneBit = BIT1;

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

std::string PathOf(const std::string& url) { return url.substr(0, url.find_first_of("?#")); }

bool IsHlsUrl(const std::string& url) {
    std::string p = PathOf(url);
    return p.size() >= 5 && p.compare(p.size() - 5, 5, ".m3u8") == 0;
}

// Number of bytes of a leading ID3v2 tag (0 when there is none).
size_t Id3Size(const uint8_t* d, size_t len) {
    if (len < 10 || d[0] != 'I' || d[1] != 'D' || d[2] != '3') {
        return 0;
    }
    return 10 + ((d[6] & 0x7F) << 21 | (d[7] & 0x7F) << 14 | (d[8] & 0x7F) << 7 | (d[9] & 0x7F));
}
}  // namespace

std::string RadioPlayer::NowPlaying() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return IsPlaying() ? name_ : std::string();
}

bool RadioPlayer::IsPlaying() const { return super_running_.load(); }

bool RadioPlayer::Play(const std::string& source, std::string* error, const std::string& title) {
    std::string url = source;
    std::string name = title.empty() ? source : title;
    if (!StartsWith(source, "http://") && !StartsWith(source, "https://")) {
        if (const RadioChannel* channel = Find(source)) {
            url = channel->url;
            name = title.empty() ? channel->name : title;
        }
    }
    if (!StartsWith(url, "http://") && !StartsWith(url, "https://")) {
        if (error) {
            *error = "Unknown channel or URL: " + source;
        }
        return false;
    }
    return PlayList({{name, url}}, false, error);
}

bool RadioPlayer::PlayList(std::vector<RadioSource> sources, bool finite, std::string* error) {
    sources.erase(std::remove_if(sources.begin(), sources.end(),
                                 [](const RadioSource& s) {
                                     return !StartsWith(s.url, "http://") && !StartsWith(s.url, "https://");
                                 }),
                  sources.end());
    if (sources.empty()) {
        if (error) {
            *error = "Nothing to play";
        }
        return false;
    }

    std::lock_guard<std::recursive_mutex> control(control_mutex_);
    Stop();
    if (super_done_ == nullptr) {
        super_done_ = xSemaphoreCreateBinary();
    }
    xSemaphoreTake(super_done_, 0);  // clear a stale signal
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_ = std::move(sources);
        queue_index_ = 0;
        finite_ = finite;
    }
    super_cancel_ = false;
    skip_requested_ = false;
    interrupted_ = false;
    busy_abort_ = false;
    super_running_ = true;
    if (xTaskCreate(SuperEntry, "radio_super", 5120, this, 3, nullptr) != pdPASS) {
        super_running_ = false;
        if (error) {
            *error = "Cannot start the player";
        }
        return false;
    }
    return true;
}

bool RadioPlayer::Next(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!super_running_.load()) {
        if (error) {
            *error = "Nothing is playing";
        }
        return false;
    }
    if (queue_index_ + 1 >= queue_.size()) {
        if (error) {
            *error = "There is no other station or track in the list";
        }
        return false;
    }
    skip_requested_ = true;
    return true;
}

size_t RadioPlayer::Remaining() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!super_running_.load() || queue_index_ + 1 >= queue_.size()) {
        return 0;
    }
    return queue_.size() - queue_index_ - 1;
}

void RadioPlayer::Stop() {
    std::lock_guard<std::recursive_mutex> control(control_mutex_);
    super_cancel_ = true;
    if (super_done_ != nullptr && super_running_.load()) {
        // The supervisor stops the stream itself and signals when it has left
        xSemaphoreTake(super_done_, pdMS_TO_TICKS(25000));
    }
    StopStream();  // also covers a stream that was started without a supervisor
}

// ---------- supervisor ----------

void RadioPlayer::SuperEntry(void* arg) {
    static_cast<RadioPlayer*>(arg)->Supervise();
    vTaskDelete(nullptr);
}

bool RadioPlayer::StreamEnded() const {
    if (done_event_ == nullptr) {
        return true;
    }
    return (xEventGroupGetBits(done_event_) & (kFetchDoneBit | kDecodeDoneBit)) ==
           (kFetchDoneBit | kDecodeDoneBit);
}

void RadioPlayer::Supervise() {
    constexpr int kStartTimeoutMs = 25000;  // open + pre-buffer, counted before "ready to play"
    std::vector<RadioSource> sources;
    bool finite;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sources = queue_;
        finite = finite_;
    }
    auto& app = Application::GetInstance();
    auto announce = [&app](const std::string& text) {
        app.Schedule([text]() { Board::GetInstance().GetDisplay()->SetChatMessage("system", text.c_str()); });
    };

    bool ever_audible = false;
    for (size_t i = 0; i < sources.size() && !super_cancel_.load();) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_index_ = i;
        }
        std::string error;
        bool advance = true;  // default: this entry failed, try the next one
        bool audible = false;
        if (StartStream(sources[i].url, sources[i].name, &error)) {
            // Phase 1: wait for sound; give up on streams that never open or never buffer
            int waited_ms = 0;
            while (!super_cancel_.load() && !skip_requested_.load() && !busy_abort_.load()) {
                if (started_audio_.load()) {
                    audible = true;
                    break;
                }
                if (StreamEnded()) {
                    break;
                }
                if (!ready_to_play_.load()) {
                    waited_ms += 100;
                    if (waited_ms >= kStartTimeoutMs) {
                        ESP_LOGW(TAG, "%s did not start within %d s", sources[i].name.c_str(), kStartTimeoutMs / 1000);
                        break;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            // Phase 2: playing
            if (audible) {
                ever_audible = true;
                while (!super_cancel_.load() && !skip_requested_.load() && !StreamEnded()) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
                // A track that played to its end is a normal finish
                if (finite && !super_cancel_.load() && !skip_requested_.load() && !interrupted_.load()) {
                    advance = false;
                }
            }
            StopStream();
        }

        if (super_cancel_.load() || interrupted_.load() || busy_abort_.load()) {
            break;  // user or device took over: never switch on our own then
        }
        const bool manual = skip_requested_.exchange(false);
        if (!advance && !manual) {
            break;  // finished normally
        }
        i++;
        if (i < sources.size()) {
            ESP_LOGW(TAG, "%s -> next: %s", sources[i - 1].name.c_str(), sources[i].name.c_str());
            announce("Doi sang: " + sources[i].name);
        }
    }
    if (!ever_audible && !super_cancel_.load() && !interrupted_.load() && !busy_abort_.load()) {
        ESP_LOGW(TAG, "None of the %u candidates could be played", static_cast<unsigned>(sources.size()));
        announce("Khong phat duoc, thu lai sau");
    }
    super_running_ = false;
    xSemaphoreGive(super_done_);
}

// ---------- one stream ----------

bool RadioPlayer::StartStream(const std::string& url, const std::string& name, std::string* error) {
    StopStream();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source_url_ = url;
        name_ = name;
    }
    cancel_ = false;
    fetch_done_ = false;
    started_audio_ = false;
    ready_to_play_ = false;
    stream_type_ = kTypeUnknown;

    stream_ = xStreamBufferCreateWithCaps(kStreamBufferSize, 1, MALLOC_CAP_SPIRAM);
    if (stream_ == nullptr) {
        stream_ = xStreamBufferCreateWithCaps(kStreamBufferFallbackSize, 1, MALLOC_CAP_DEFAULT);
    }
    done_event_ = xEventGroupCreate();
    if (stream_ == nullptr || done_event_ == nullptr) {
        if (error) {
            *error = "Out of memory";
        }
        Cleanup();
        return false;
    }

    running_ = true;
    if (xTaskCreate(FetchEntry, "radio_fetch", 8192, this, 3, nullptr) != pdPASS ||
        xTaskCreate(DecodeEntry, "radio_decode", 8192, this, 4, nullptr) != pdPASS) {
        cancel_ = true;
        xEventGroupSetBits(done_event_, kFetchDoneBit | kDecodeDoneBit);
        if (error) {
            *error = "Cannot start player tasks";
        }
        StopStream();
        return false;
    }
    ESP_LOGI(TAG, "Playing %s (%s)", name.c_str(), url.c_str());
    return true;
}

void RadioPlayer::StopStream() {
    if (done_event_ == nullptr) {
        return;
    }
    cancel_ = true;
    // Wakes a decode task blocked in PushPcmToPlaybackQueue() and drops queued audio
    Application::GetInstance().GetAudioService().ResetDecoder();
    xEventGroupWaitBits(done_event_, kFetchDoneBit | kDecodeDoneBit, pdFALSE, pdTRUE,
                        pdMS_TO_TICKS(15000));
    Cleanup();
}

void RadioPlayer::Cleanup() {
    running_ = false;
    if (stream_ != nullptr) {
        vStreamBufferDeleteWithCaps(stream_);
        stream_ = nullptr;
    }
    if (done_event_ != nullptr) {
        vEventGroupDelete(done_event_);
        done_event_ = nullptr;
    }
}

bool RadioPlayer::ShouldStop() const { return cancel_.load(); }

// ==================== Fetch side ====================

void RadioPlayer::FetchEntry(void* arg) {
    static_cast<RadioPlayer*>(arg)->FetchTask();
    vTaskDelete(nullptr);
}

void RadioPlayer::FetchTask() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = source_url_;
    }
    bool ok = IsHlsUrl(url) ? FetchHls(url) : FetchDirect(url);
    if (!ok && !ShouldStop()) {
        ESP_LOGW(TAG, "Stream ended with an error");
    }
    fetch_done_ = true;
    xEventGroupSetBits(done_event_, kFetchDoneBit);
}

bool RadioPlayer::WriteBytes(const uint8_t* data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        if (ShouldStop()) {
            return false;
        }
        offset += xStreamBufferSend(stream_, data + offset, len - offset, pdMS_TO_TICKS(500));
    }
    return true;
}

// Detects the elementary stream type from the first bytes of a segment / stream.
static int DetectStreamType(const uint8_t* d, size_t len, const std::string& url) {
    size_t skip = Id3Size(d, len);
    if (skip > 0 && skip + 2 <= len) {
        d += skip;
        len -= skip;
    }
    if (len >= 1 && d[0] == 0x47) {
        return 3;  // MPEG-TS sync byte -> kTypeTs
    }
    if (len >= 2 && d[0] == 0xFF) {
        if ((d[1] & 0xF6) == 0xF0) {
            return 1;  // ADTS AAC -> kTypeAac
        }
        if ((d[1] & 0xE0) == 0xE0) {
            return 2;  // MPEG audio -> kTypeMp3
        }
    }
    std::string p = PathOf(url);
    auto ends = [&](const char* ext) {
        size_t n = strlen(ext);
        return p.size() >= n && p.compare(p.size() - n, n, ext) == 0;
    };
    if (ends(".ts")) return 3;
    if (ends(".aac")) return 1;
    if (ends(".mp3")) return 2;
    return 0;
}

bool RadioPlayer::DownloadInto(const std::string& url, bool detect_type_from_body) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!http) {
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Accept-Encoding", "identity");
    auto opened = http->Open("GET", url);
    if (!opened) {
        ESP_LOGW(TAG, "Open %s failed: %s", url.c_str(), opened.error().ToString().c_str());
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status < 200 || *status >= 300) {
        ESP_LOGW(TAG, "GET %s -> %d", url.c_str(), status ? *status : -1);
        http->Close();
        return false;
    }

    bool first = true;
    bool ok = true;
    std::vector<char> buffer(kHttpChunk);
    while (!ShouldStop()) {
        auto size = http->Read(buffer.data(), buffer.size());
        if (!size) {
            ESP_LOGW(TAG, "Read failed: %s", size.error().ToString().c_str());
            ok = false;
            break;
        }
        if (*size == 0) {
            break;  // end of this segment / stream
        }
        const uint8_t* data = reinterpret_cast<const uint8_t*>(buffer.data());
        size_t len = *size;
        if (first) {
            first = false;
            if (detect_type_from_body && stream_type_.load() == kTypeUnknown) {
                int type = DetectStreamType(data, len, url);
                if (type == kTypeUnknown) {
                    ESP_LOGE(TAG, "Unsupported stream format at %s", url.c_str());
                    ok = false;
                    break;
                }
                stream_type_ = type;
            }
            size_t skip = Id3Size(data, len);  // drop a leading ID3 tag, decoders do not parse it
            if (skip > 0) {
                if (skip >= len) {
                    continue;
                }
                data += skip;
                len -= skip;
            }
        }
        if (!WriteBytes(data, len)) {
            ok = false;
            break;
        }
    }
    http->Close();
    return ok;
}

bool RadioPlayer::FetchDirect(const std::string& url) {
    for (int attempt = 0; attempt <= kMaxDirectReconnects && !ShouldStop(); attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Reconnecting stream (%d/%d)", attempt, kMaxDirectReconnects);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        bool ok = DownloadInto(url, true);
        if (ok && !ShouldStop()) {
            continue;  // server closed a live stream: reconnect
        }
    }
    return ShouldStop();
}

bool RadioPlayer::FetchHls(const std::string& start_url) {
    std::string playlist_url = start_url;
    long long next_seq = -1;
    int failures = 0;
    std::vector<std::string> recent;  // guards against replaying a segment after a playlist glitch

    while (!ShouldStop()) {
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
        std::string body;
        if (http) {
            http->SetTimeout(kHttpTimeoutMs);
            http->SetHeader("Accept-Encoding", "identity");
            auto opened = http->Open("GET", playlist_url);
            if (opened) {
                auto status = http->GetStatusCode();
                if (status && *status >= 200 && *status < 300) {
                    body = http->ReadAll();
                }
                http->Close();
            }
        }
        if (body.empty() || body.rfind("#EXTM3U", 0) != 0) {
            if (++failures >= kMaxPlaylistFailures) {
                ESP_LOGE(TAG, "Playlist %s unavailable", playlist_url.c_str());
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        failures = 0;

        HlsPlaylist playlist = ParseHlsPlaylist(body, playlist_url);
        if (playlist.is_master) {  // follow the first variant
            playlist_url = playlist.variant_url;
            continue;
        }
        const long long media_seq = playlist.media_sequence;
        const int target_duration = playlist.target_duration;
        const bool ended = playlist.ended;
        const std::vector<std::string>& segments = playlist.segments;
        if (segments.empty()) {
            return false;
        }
        if (playlist.has_fmp4) {
            ESP_LOGE(TAG, "fMP4 HLS segments are not supported");
            return false;
        }

        if (next_seq < 0) {
            // Live streams: start close to the live edge to keep latency low
            size_t first_index = ended || segments.size() <= 3 ? 0 : segments.size() - 3;
            next_seq = media_seq + static_cast<long long>(first_index);
        }

        bool downloaded_any = false;
        for (size_t i = 0; i < segments.size() && !ShouldStop(); i++) {
            long long seq = media_seq + static_cast<long long>(i);
            if (seq < next_seq) {
                continue;
            }
            if (std::find(recent.begin(), recent.end(), segments[i]) != recent.end()) {
                next_seq = seq + 1;
                continue;
            }
            if (!DownloadInto(segments[i], true)) {
                if (ShouldStop()) {
                    return true;
                }
                ESP_LOGW(TAG, "Segment %lld failed, skipping", seq);
            }
            recent.push_back(segments[i]);
            if (recent.size() > 16) {
                recent.erase(recent.begin());
            }
            next_seq = seq + 1;
            downloaded_any = true;
        }
        if (ended) {
            return true;
        }
        int wait_ms = downloaded_any ? 500 : std::clamp(target_duration * 1000 / 2, 1000, 5000);
        for (int waited = 0; waited < wait_ms && !ShouldStop(); waited += 100) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    return true;
}

// ==================== Decode side ====================

void RadioPlayer::DecodeEntry(void* arg) {
    static_cast<RadioPlayer*>(arg)->DecodeTask();
    vTaskDelete(nullptr);
}

bool RadioPlayer::OpenDecoder(StreamType type) {
    static bool registered = false;
    if (!registered) {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        registered = true;
    }
    esp_audio_simple_dec_cfg_t cfg = {};
    switch (type) {
        case kTypeAac:
            cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
            break;
        case kTypeMp3:
            cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
            break;
        case kTypeTs:
            cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_TS;
            break;
        default:
            return false;
    }
    esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &decoder_);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Cannot open decoder (type %d): %d", static_cast<int>(type), static_cast<int>(ret));
        decoder_ = nullptr;
        return false;
    }
    out_buffer_.assign(8192, 0);
    return true;
}

void RadioPlayer::EmitPcm(const uint8_t* data, size_t bytes) {
    esp_audio_simple_dec_info_t info = {};
    if (esp_audio_simple_dec_get_info(decoder_, &info) != ESP_AUDIO_ERR_OK || info.sample_rate == 0) {
        return;
    }
    const int channels = std::max<int>(1, info.channel);
    const int16_t* in = reinterpret_cast<const int16_t*>(data);
    const size_t frames = bytes / sizeof(int16_t) / channels;
    if (frames == 0) {
        return;
    }

    std::vector<int16_t> mono(frames);
    for (size_t i = 0; i < frames; i++) {
        int sum = 0;
        for (int c = 0; c < channels; c++) {
            sum += in[i * channels + c];
        }
        mono[i] = static_cast<int16_t>(sum / channels);
    }

    auto* codec = Board::GetInstance().GetAudioCodec();
    const int target_rate = codec->output_sample_rate();
    if (static_cast<int>(info.sample_rate) != target_rate) {
        if (resampler_ == nullptr || resampler_src_rate_ != static_cast<int>(info.sample_rate)) {
            if (resampler_ != nullptr) {
                esp_ae_rate_cvt_close(resampler_);
                resampler_ = nullptr;
            }
            esp_ae_rate_cvt_cfg_t cfg = {
                .src_rate = static_cast<uint32_t>(info.sample_rate),
                .dest_rate = static_cast<uint32_t>(target_rate),
                .channel = 1,
                .bits_per_sample = ESP_AUDIO_BIT16,
                .complexity = 2,
                .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
            };
            esp_ae_rate_cvt_open(&cfg, &resampler_);
            resampler_src_rate_ = static_cast<int>(info.sample_rate);
            if (resampler_ == nullptr) {
                ESP_LOGE(TAG, "Cannot create resampler %d -> %d", resampler_src_rate_, target_rate);
                return;
            }
        }
        uint32_t max_out = 0;
        esp_ae_rate_cvt_get_max_out_sample_num(resampler_, mono.size(), &max_out);
        std::vector<int16_t> resampled(max_out);
        uint32_t actual = max_out;
        esp_ae_rate_cvt_process(resampler_, reinterpret_cast<esp_ae_sample_t>(mono.data()), mono.size(),
                                reinterpret_cast<esp_ae_sample_t>(resampled.data()), &actual);
        resampled.resize(actual);
        mono = std::move(resampled);
    }

    if (pcm_observer_) {
        pcm_observer_(mono.data(), mono.size(), target_rate);
    }
    if (!Application::GetInstance().GetAudioService().PushPcmToPlaybackQueue(std::move(mono), true)) {
        cancel_ = true;  // stopped or reset while waiting for room
    }
}

void RadioPlayer::DecodeTask() {
    auto& app = Application::GetInstance();
    std::string name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        name = name_;
    }

    // 1. Wait until the fetcher has seen the stream type
    while (!ShouldStop() && stream_type_.load() == kTypeUnknown) {
        if (fetch_done_.load()) {
            ESP_LOGE(TAG, "Nothing to play");
            cancel_ = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    bool started = false;
    if (!ShouldStop() && OpenDecoder(static_cast<StreamType>(stream_type_.load()))) {
        // 2. Pre-buffer
        while (!ShouldStop() && !fetch_done_.load() && xStreamBufferBytesAvailable(stream_) < kPrebufferBytes) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        ready_to_play_ = true;  // opened and buffered: from here on only the device state can delay us

        // 3. Wait for the conversation to end. A voice command ("bat radio VOV1") arrives
        //    while the device is Speaking/Listening: once the reply has been spoken and the
        //    device sits in Listening for a moment, close the session so the radio can play.
        int waited_ms = 0;
        int listening_ms = 0;
        bool closed_session = false;
        while (!ShouldStop() && app.GetDeviceState() != kDeviceStateIdle) {
            if (app.GetDeviceState() == kDeviceStateListening) {
                listening_ms += 100;
                if (listening_ms >= kListeningGraceMs && !closed_session) {
                    closed_session = true;
                    app.CloseConversation();  // closes the channel -> Idle (never starts a chat)
                }
            } else {
                listening_ms = 0;
            }
            waited_ms += 100;
            if (waited_ms > kWaitIdleTimeoutMs) {
                ESP_LOGW(TAG, "Device never became idle, giving up");
                busy_abort_ = true;
                cancel_ = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (!ShouldStop()) {
            started = true;
            started_audio_ = true;
            app.Schedule([name]() {
                Board::GetInstance().GetDisplay()->SetChatMessage("system", ("Radio: " + name).c_str());
            });
            if (state_observer_) {
                state_observer_(name, true);
            }
        }

        // 4. Decode and play
        std::vector<uint8_t> pending;
        std::vector<uint8_t> chunk(4096);
        while (!ShouldStop()) {
            if (app.GetDeviceState() != kDeviceStateIdle) {
                ESP_LOGI(TAG, "Device left idle state, stopping radio");
                interrupted_ = true;
                cancel_ = true;
                break;
            }
            size_t got = xStreamBufferReceive(stream_, chunk.data(), chunk.size(), pdMS_TO_TICKS(200));
            if (got == 0) {
                if (fetch_done_.load() && xStreamBufferIsEmpty(stream_)) {
                    break;
                }
                continue;
            }
            pending.insert(pending.end(), chunk.begin(), chunk.begin() + got);

            esp_audio_simple_dec_raw_t raw = {};
            raw.buffer = pending.data();
            raw.len = pending.size();
            while (raw.len > 0 && !ShouldStop()) {
                esp_audio_simple_dec_out_t out = {};
                out.buffer = out_buffer_.data();
                out.len = out_buffer_.size();
                esp_audio_err_t ret = esp_audio_simple_dec_process(decoder_, &raw, &out);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    out_buffer_.resize(out.needed_size);
                    continue;
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    ESP_LOGW(TAG, "Decode error %d, dropping %u bytes", static_cast<int>(ret),
                             static_cast<unsigned>(raw.len));
                    raw.consumed = raw.len;
                }
                if (out.decoded_size > 0) {
                    EmitPcm(out.buffer, out.decoded_size);
                }
                if (raw.consumed == 0 && out.decoded_size == 0) {
                    break;  // needs more input
                }
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
            }
            size_t used = pending.size() - raw.len;
            pending.erase(pending.begin(), pending.begin() + used);
            if (pending.size() > 16 * 1024) {
                pending.clear();  // undecodable garbage, resynchronise on the next bytes
            }
        }
    }

    if (decoder_ != nullptr) {
        esp_audio_simple_dec_close(decoder_);
        decoder_ = nullptr;
    }
    if (resampler_ != nullptr) {
        esp_ae_rate_cvt_close(resampler_);
        resampler_ = nullptr;
    }
    cancel_ = true;  // lets the fetcher finish
    if (started) {
        app.Schedule([]() { Board::GetInstance().GetDisplay()->SetChatMessage("system", ""); });
        if (state_observer_) {
            state_observer_(name, false);
        }
    }
    xEventGroupSetBits(done_event_, kDecodeDoneBit);
}
