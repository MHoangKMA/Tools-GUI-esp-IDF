#ifndef SONG_DISPLAY_H
#define SONG_DISPLAY_H

#include <atomic>
#include <mutex>
#include <string>

#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Second OLED (0.96" SSD1306 128x64) dedicated to music: the song / radio title (scrolls when
// long, Vietnamese accents are folded to ASCII because the built-in 5x7 font has no accents)
// and a 16-band spectrum fed with the PCM that is being played. Independent of LVGL, which
// drives the emotion OLED.
class SongDisplay {
public:
    SongDisplay(i2c_master_bus_handle_t bus, uint8_t address, bool mirror_x, bool mirror_y);
    ~SongDisplay();

    // False when the panel does not answer (display absent): every other call is then a no-op.
    bool Init();

    void SetPlaying(const std::string& title, bool playing);
    // Mono PCM at `sample_rate` (called from the player task for every decoded frame)
    void FeedPcm(const int16_t* samples, size_t count, int sample_rate);

    static std::string FoldToAscii(const std::string& utf8);

    static constexpr int kWidth = 128;
    static constexpr int kHeight = 64;
    static constexpr int kBands = 16;
    static constexpr int kFftSize = 256;

private:
    static void RenderEntry(void* arg);
    void RenderLoop();
    void DrawFrame(bool playing, const std::string& title, int scroll_px);
    void Clear();
    void SetPixel(int x, int y);
    void FillRect(int x, int y, int w, int h);
    int DrawText(int x, int y, const std::string& text, int scale);
    void ComputeSpectrum();

    i2c_master_bus_handle_t bus_;
    uint8_t address_;
    bool mirror_x_;
    bool mirror_y_;
    esp_lcd_panel_io_handle_t io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    TaskHandle_t task_ = nullptr;
    bool ready_ = false;

    uint8_t frame_[kWidth * kHeight / 8];

    std::mutex mutex_;
    std::string title_;
    bool playing_ = false;

    // Spectrum
    float samples_[kFftSize] = {};
    int sample_fill_ = 0;
    int hop_counter_ = 0;
    float re_[kFftSize];
    float im_[kFftSize];
    std::atomic<uint8_t> bands_[kBands];
};

#endif  // SONG_DISPLAY_H
