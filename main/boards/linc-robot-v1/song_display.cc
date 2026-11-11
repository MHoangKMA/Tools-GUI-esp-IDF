#include "song_display.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <wifi_manager.h>

#include "ascii_fold.h"
#include "settings.h"

#define TAG "SongDisplay"

namespace {

// Classic 5x7 font, ASCII 32..126, one byte per column, bit 0 = top row.
const uint8_t kFont5x7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x56, 0x20, 0x50}, {0x00, 0x08, 0x07, 0x03, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x2A, 0x1C, 0x7F, 0x1C, 0x2A}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x80, 0x70, 0x30, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x00, 0x60, 0x60, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x72, 0x49, 0x49, 0x49, 0x46}, {0x21, 0x41, 0x49, 0x4D, 0x33}, {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x31}, {0x41, 0x21, 0x11, 0x09, 0x07},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x46, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x00, 0x14, 0x00, 0x00},
    {0x00, 0x40, 0x34, 0x00, 0x00}, {0x00, 0x08, 0x14, 0x22, 0x41}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x59, 0x09, 0x06}, {0x3E, 0x41, 0x5D, 0x59, 0x4E},
    {0x7C, 0x12, 0x11, 0x12, 0x7C}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x41, 0x51, 0x73}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x1C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x26, 0x49, 0x49, 0x49, 0x32}, {0x03, 0x01, 0x7F, 0x01, 0x03}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x59, 0x49, 0x4D, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x41},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x41, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x03, 0x07, 0x08, 0x00}, {0x20, 0x54, 0x54, 0x78, 0x40},
    {0x7F, 0x28, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x28}, {0x38, 0x44, 0x44, 0x28, 0x7F},
    {0x38, 0x54, 0x54, 0x54, 0x18}, {0x00, 0x08, 0x7E, 0x09, 0x02}, {0x18, 0xA4, 0xA4, 0x9C, 0x78},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x40, 0x3D, 0x00},
    {0x7F, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x78, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38}, {0xFC, 0x18, 0x24, 0x24, 0x18},
    {0x18, 0x24, 0x24, 0x18, 0xFC}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x24},
    {0x04, 0x04, 0x3F, 0x44, 0x24}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C},
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x4C, 0x90, 0x90, 0x90, 0x7C},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00}, {0x00, 0x00, 0x77, 0x00, 0x00},
    {0x00, 0x41, 0x36, 0x08, 0x00}, {0x02, 0x01, 0x02, 0x04, 0x02},
};

constexpr int kCharWidth = 6;  // 5 columns + 1 spacing
constexpr int kBarAreaTop = 26;
// One frame is ~1 KB over I2C (~26 ms at 400 kHz) and the bus is shared with the emotion OLED,
// so keep the song screen at 10 frames per second.
constexpr int kFrameIntervalMs = 100;
constexpr int kScrollStepPx = 4;

}  // namespace

std::string SongDisplay::FoldToAscii(const std::string& utf8) { return AsciiFold(utf8); }

SongDisplay::SongDisplay(i2c_master_bus_handle_t bus, uint8_t address, bool mirror_x, bool mirror_y)
    : bus_(bus), address_(address), mirror_x_(mirror_x), mirror_y_(mirror_y) {
    memset(frame_, 0, sizeof(frame_));
    for (auto& band : bands_) {
        band = 0;
    }
}

SongDisplay::~SongDisplay() {
    if (task_ != nullptr) {
        vTaskDelete(task_);
    }
    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (io_ != nullptr) {
        esp_lcd_panel_io_del(io_);
    }
}

bool SongDisplay::Init() {
    if (i2c_master_probe(bus_, address_, 100) != ESP_OK) {
        ESP_LOGW(TAG, "No OLED answers at 0x%02X, song display disabled", address_);
        return false;
    }

    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = address_,
        .scl_speed_hz = 400 * 1000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = nullptr,
        .user_ctx = nullptr,
        .flags = {.dc_low_on_data = 0, .disable_control_phase = 0},
    };
    if (esp_lcd_new_panel_io_i2c(bus_, &io_config, &io_) != ESP_OK) {
        return false;
    }

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.bits_per_pixel = 1;
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {.height = static_cast<uint8_t>(kHeight)};
    panel_config.vendor_config = &ssd1306_config;
    if (esp_lcd_new_panel_ssd1306(io_, &panel_config, &panel_) != ESP_OK) {
        return false;
    }
    esp_lcd_panel_reset(panel_);
    if (esp_lcd_panel_init(panel_) != ESP_OK) {
        ESP_LOGE(TAG, "Panel init failed");
        return false;
    }
    esp_lcd_panel_mirror(panel_, mirror_x_, mirror_y_);
    esp_lcd_panel_disp_on_off(panel_, true);
    ready_ = true;

    DrawFrame(false, "", 0);
    xTaskCreate(RenderEntry, "song_oled", 4096, this, 1, &task_);
    return true;
}

void SongDisplay::SetPlaying(const std::string& title, bool playing) {
    std::lock_guard<std::mutex> lock(mutex_);
    title_ = FoldToAscii(title);
    playing_ = playing;
    if (!playing) {
        for (auto& band : bands_) {
            band = 0;
        }
        sample_fill_ = 0;
    }
}

// ---------- spectrum ----------

void SongDisplay::FeedPcm(const int16_t* samples, size_t count, int sample_rate) {
    if (!ready_) {
        return;
    }
    // Collect the newest kFftSize samples; one spectrum about every 40 ms of audio
    for (size_t i = 0; i < count; i++) {
        samples_[sample_fill_++] = samples[i] / 32768.0f;
        if (sample_fill_ == kFftSize) {
            hop_counter_ += kFftSize;
            if (hop_counter_ >= sample_rate / 25) {
                hop_counter_ = 0;
                ComputeSpectrum();
            }
            sample_fill_ = 0;
        }
    }
}

void SongDisplay::ComputeSpectrum() {
    // Hann window
    for (int i = 0; i < kFftSize; i++) {
        float w = 0.5f - 0.5f * cosf(2.0f * static_cast<float>(M_PI) * i / (kFftSize - 1));
        re_[i] = samples_[i] * w;
        im_[i] = 0.0f;
    }
    // In-place radix-2 FFT
    for (int i = 1, j = 0; i < kFftSize; i++) {
        int bit = kFftSize >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(re_[i], re_[j]);
            std::swap(im_[i], im_[j]);
        }
    }
    for (int len = 2; len <= kFftSize; len <<= 1) {
        float angle = -2.0f * static_cast<float>(M_PI) / len;
        float wr = cosf(angle);
        float wi = sinf(angle);
        for (int i = 0; i < kFftSize; i += len) {
            float cr = 1.0f;
            float ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k;
                int b = i + k + len / 2;
                float tr = re_[b] * cr - im_[b] * ci;
                float ti = re_[b] * ci + im_[b] * cr;
                re_[b] = re_[a] - tr;
                im_[b] = im_[a] - ti;
                re_[a] += tr;
                im_[a] += ti;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
    // Log-spaced bands over bins 1..127 (about 90 Hz .. Nyquist)
    const int usable = kFftSize / 2 - 1;
    for (int b = 0; b < kBands; b++) {
        int lo = static_cast<int>(powf(static_cast<float>(usable), static_cast<float>(b) / kBands));
        int hi = static_cast<int>(powf(static_cast<float>(usable), static_cast<float>(b + 1) / kBands));
        lo = std::max(lo, 1);
        hi = std::max(hi, lo);
        float peak = 0.0f;
        for (int k = lo; k <= std::min(hi, usable); k++) {
            peak = std::max(peak, sqrtf(re_[k] * re_[k] + im_[k] * im_[k]));
        }
        // 0 dBFS full-scale sine ~ kFftSize/4 after the Hann window
        float db = 20.0f * log10f(peak / (kFftSize / 4.0f) + 1e-6f);
        float level = std::clamp((db + 60.0f) / 54.0f, 0.0f, 1.0f);  // -60 dB .. -6 dB
        uint8_t target = static_cast<uint8_t>(level * 100.0f);
        uint8_t previous = bands_[b].load();
        // Fast attack, slow release
        bands_[b] = target > previous ? target : static_cast<uint8_t>(previous * 0.8f);
    }
}

// ---------- drawing ----------

void SongDisplay::Clear() { memset(frame_, 0, sizeof(frame_)); }

void SongDisplay::SetPixel(int x, int y) {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) {
        return;
    }
    frame_[(y >> 3) * kWidth + x] |= static_cast<uint8_t>(1u << (y & 7));
}

void SongDisplay::FillRect(int x, int y, int w, int h) {
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            SetPixel(xx, yy);
        }
    }
}

// Draws `text` at (x, y); returns the x position after the last character.
int SongDisplay::DrawText(int x, int y, const std::string& text, int scale) {
    for (char ch : text) {
        if (x >= kWidth) {
            break;
        }
        int index = (ch >= 32 && ch < 127) ? ch - 32 : '?' - 32;
        for (int col = 0; col < 5; col++) {
            uint8_t bits = kFont5x7[index][col];
            for (int row = 0; row < 8; row++) {
                if (bits & (1u << row)) {
                    FillRect(x + col * scale, y + row * scale, scale, scale);
                }
            }
        }
        x += kCharWidth * scale;
    }
    return x;
}

void SongDisplay::DrawFrame(bool playing, const std::string& title, int scroll_px) {
    Clear();
    if (!playing) {
        DrawText(4, 6, "Robot", 2);
        std::string ip = WifiManager::GetInstance().GetIpAddress();
        DrawText(4, 32, ip.empty() ? "Dang ket noi Wi-Fi" : "Web: " + ip + ":8080", 1);
        // Firmware-upload key (see robot_web_server.cc): visible only to someone at the robot
        std::string key = Settings("robot", false).GetString("ota_token");
        DrawText(4, 46, key.empty() ? std::string("San sang nghe nhac") : "OTA key: " + key, 1);
    } else {
        DrawText(0, 0, "DANG PHAT", 1);
        // Title line: scroll when wider than the screen
        const int text_width = static_cast<int>(title.size()) * kCharWidth;
        if (text_width <= kWidth) {
            DrawText(0, 12, title, 1);
        } else {
            const std::string loop = title + "    ";
            const int loop_width = static_cast<int>(loop.size()) * kCharWidth;
            const int offset = scroll_px % loop_width;
            DrawText(-offset, 12, loop, 1);
            DrawText(loop_width - offset, 12, loop, 1);
        }
        // Spectrum: 16 bars of 6 px with 2 px gaps
        const int area = kHeight - kBarAreaTop;
        for (int b = 0; b < kBands; b++) {
            int h = std::max(1, bands_[b].load() * area / 100);
            FillRect(b * 8, kHeight - h, 6, h);
        }
    }
    esp_lcd_panel_draw_bitmap(panel_, 0, 0, kWidth, kHeight, frame_);
}

void SongDisplay::RenderEntry(void* arg) {
    static_cast<SongDisplay*>(arg)->RenderLoop();
    vTaskDelete(nullptr);
}

void SongDisplay::RenderLoop() {
    int scroll_px = 0;
    int idle_ticks = 0;
    std::string last_title;
    while (true) {
        bool playing;
        std::string title;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            playing = playing_;
            title = title_;
        }
        if (title != last_title) {
            scroll_px = 0;
            last_title = title;
        }
        if (playing) {
            DrawFrame(true, title, scroll_px);
            scroll_px += kScrollStepPx;
            vTaskDelay(pdMS_TO_TICKS(kFrameIntervalMs));
        } else {
            // Idle screen only changes when the IP does: refresh once a second
            if (idle_ticks++ % 10 == 0) {
                DrawFrame(false, "", 0);
            }
            vTaskDelay(pdMS_TO_TICKS(kFrameIntervalMs));
        }
    }
}
