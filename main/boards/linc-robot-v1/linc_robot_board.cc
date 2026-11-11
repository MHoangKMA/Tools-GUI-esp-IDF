#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "drv8833_motor.h"
#include "eyes_display.h"
#include "eyes_lcd_display.h"
#include "radio_player.h"
#include "robot_controller.h"
#include <cmath>
#include <vector>

#include "robot_ap.h"
#include "robot_selftest.h"
#include "robot_web_server.h"
#include "settings.h"
#include "song_display.h"
#include "turn_signals.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_sh1106.h>
#include <esp_lcd_panel_io.h>
#include <driver/spi_common.h>

#define TAG "LincRobotV1"

// Robot with two OLEDs:
//   emotion screen: 1.3" colour TFT ST7789 over SPI (default, EyesLcdDisplay) or a 1.3" OLED on the
//   shared I2C bus at 0x3D (EyesOledDisplay): two-eye face + status bar + one chat line
//   song OLED    (0.96", 0x3C) -> song / radio title + spectrum (SongDisplay)
// Both sit on the same I2C bus, so their addresses must differ (see config.h).
class LincRobotV1Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;  // shared by both OLEDs
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    SongDisplay* song_display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Drv8833Motor motor_;

    static i2c_master_bus_handle_t NewI2cBus(i2c_port_t port, gpio_num_t sda, gpio_num_t scl) {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = port,
            .sda_io_num = sda,
            .scl_io_num = scl,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        i2c_master_bus_handle_t bus = nullptr;
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));
        return bus;
    }

    // Creates the shared bus and logs every device on it, which makes address mix-ups obvious
    void InitializeI2cBus() {
        i2c_bus_ = NewI2cBus((i2c_port_t)0, OLED_I2C_SDA_PIN, OLED_I2C_SCL_PIN);
        int found = 0;
        for (uint8_t address = 0x08; address < 0x78; address++) {
            if (i2c_master_probe(i2c_bus_, address, 20) == ESP_OK) {
                ESP_LOGI(TAG, "I2C device found at 0x%02X", address);
                found++;
            }
        }
        if (found == 0) {
            ESP_LOGW(TAG, "No I2C device on SDA %d / SCL %d: check wiring and pull-ups", OLED_I2C_SDA_PIN, OLED_I2C_SCL_PIN);
        }
    }

    void InitializeEmotionOled() {
        // A missing / mis-wired screen must not stop the robot from booting
        if (i2c_master_probe(i2c_bus_, EMOTION_OLED_ADDR, 100) != ESP_OK) {
            ESP_LOGE(TAG, "Emotion OLED not found at 0x%02X, running without it (see the I2C scan above)",
                     EMOTION_OLED_ADDR);
            display_ = new NoDisplay();
            return;
        }

        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = EMOTION_OLED_ADDR,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;
        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#if EMOTION_OLED_IS_SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize emotion display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        // Face with two eyes (blinks, glances, follows the emotion names) instead of an emoji
        display_ = new EyesOledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X,
                                       DISPLAY_MIRROR_Y);
    }

    // 1.3" colour IPS TFT (ST7789, SPI, no CS pin): the two-eye face in colour
    void InitializeEmotionTft() {
        spi_bus_config_t bus = {};
        bus.mosi_io_num = TFT_MOSI_PIN;
        bus.miso_io_num = GPIO_NUM_NC;
        bus.sclk_io_num = TFT_CLK_PIN;
        bus.quadwp_io_num = GPIO_NUM_NC;
        bus.quadhd_io_num = GPIO_NUM_NC;
        bus.max_transfer_sz = TFT_WIDTH * TFT_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = TFT_CS_PIN;
        io_config.dc_gpio_num = TFT_DC_PIN;
        io_config.spi_mode = TFT_SPI_MODE;
        io_config.pclk_hz = TFT_PCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = TFT_RST_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, TFT_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, TFT_SWAP_XY);
        esp_lcd_panel_mirror(panel, TFT_MIRROR_X, TFT_MIRROR_Y);

        display_ = new EyesLcdDisplay(panel_io, panel, TFT_WIDTH, TFT_HEIGHT, TFT_OFFSET_X, TFT_OFFSET_Y,
                                      TFT_MIRROR_X, TFT_MIRROR_Y, TFT_SWAP_XY);
    }

    void InitializeEmotionDisplay() {
#if EMOTION_DISPLAY_TFT
        InitializeEmotionTft();
#else
        InitializeEmotionOled();
#endif
    }

    void InitializeSongDisplay() {
        song_display_ = new SongDisplay(i2c_bus_, SONG_OLED_ADDR, SONG_OLED_MIRROR_X, SONG_OLED_MIRROR_Y);
        if (!song_display_->Init()) {
            delete song_display_;
            song_display_ = nullptr;
        }
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        touch_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    // Wheels, turn signals, MCP tools (voice control, radio), local web page, song screen
    void InitializeRobot() {
        auto& robot = RobotController::GetInstance();
        robot.Initialize(&motor_);
        robot.RegisterMcpTools();
        // The two LEDs are the turn signals: they follow the wheels (RobotController), blink on a
        // command (self.robot.lights, web page) and flash with some faces (the observer below)
        auto& signals = TurnSignals::GetInstance();
        signals.Initialize(LEFT_SIGNAL_GPIO, RIGHT_SIGNAL_GPIO, SIGNAL_ACTIVE_LOW);
        signals.SetEffectsEnabled(LIGHT_EFFECTS_ENABLED);
        EyesFace::SetEmotionObserver([](const std::string& emotion) {
            TurnSignals::GetInstance().OnEmotion(emotion);
        });

        RobotWebServer::EnsureAdminKey();  // shown on the song OLED, needed for admin actions
        RobotWebServer::GetInstance().Initialize();
        RobotAp::GetInstance().Initialize();  // own hotspot next to the router connection

        auto& radio = RadioPlayer::GetInstance();
        radio.SetPcmObserver([this](const int16_t* samples, size_t count, int sample_rate) {
            if (song_display_ != nullptr) {
                song_display_->FeedPcm(samples, count, sample_rate);
            }
        });
        radio.SetStateObserver([this](const std::string& title, bool playing) {
            if (song_display_ != nullptr) {
                song_display_->SetPlaying(title, playing);
            }
        });
    }

public:
    LincRobotV1Board() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
        motor_(MOTOR_A_IN1_GPIO, MOTOR_A_IN2_GPIO, MOTOR_B_IN1_GPIO, MOTOR_B_IN2_GPIO,
               MOTOR_LEFT_INVERT, MOTOR_RIGHT_INVERT) {
        InitializeI2cBus();
        InitializeEmotionDisplay();
        InitializeSongDisplay();
        InitializeButtons();
        InitializeRobot();
#if EMOTION_DISPLAY_TFT
        GetBacklight()->RestoreBrightness();  // turns the TFT backlight on at the saved level
#endif
        RegisterSelfTest();
    }

    // Parts of this robot that the start-up self test goes through (see robot_selftest.h)
    void RegisterSelfTest() {
        auto& test = StartupSelfTest::GetInstance();

        test.AddStep("Man hinh bieu cam", [this]() {
            using R = StartupSelfTest::StepResult;
            if (display_ == nullptr) {
                return R{false, false, "khong khoi tao duoc"};
            }
            for (const char* face : {"happy", "thinking", "surprised", "neutral"}) {
                Application::GetInstance().Schedule([face]() { Board::GetInstance().GetDisplay()->SetEmotion(face); });
                vTaskDelay(pdMS_TO_TICKS(450));
            }
            return R{true, false, "xem man hinh lon doi mat"};
        });

        test.AddStep("Man hinh bai hat", [this]() {
            using R = StartupSelfTest::StepResult;
            if (song_display_ == nullptr) {
                return R{false, false, "khong thay OLED I2C 0x3C"};
            }
            song_display_->SetPlaying("KIEM TRA MAN HINH", true);
            // A synthetic tone makes the spectrum bars move
            std::vector<int16_t> tone(512);
            for (int round = 0; round < 8; round++) {
                for (size_t i = 0; i < tone.size(); i++) {
                    tone[i] = static_cast<int16_t>(12000.0f * std::sin(2.0f * 3.14159265f * (300.0f + 250.0f * round) *
                                                                        i / AUDIO_OUTPUT_SAMPLE_RATE));
                }
                song_display_->FeedPcm(tone.data(), tone.size(), AUDIO_OUTPUT_SAMPLE_RATE);
                vTaskDelay(pdMS_TO_TICKS(160));
            }
            song_display_->SetPlaying("", false);
            return R{true, false, "xem man hinh nho"};
        });

        test.AddStep("Den xi nhan", []() {
            using R = StartupSelfTest::StepResult;
            TurnSignals::GetInstance().SetManual(SignalPattern::kAlternate, 900);
            vTaskDelay(pdMS_TO_TICKS(1000));
            return R{true, false, "2 den nhap nhay"};
        });

        test.AddStep("Banh xe", []() {
            using R = StartupSelfTest::StepResult;
            Settings settings("robot", false);
            if (!settings.GetBool("selftest_motor", true)) {
                return R{true, true, "tat trong cai dat"};
            }
            // Spin in place for a fraction of a second: the robot does not travel
            auto& robot = RobotController::GetInstance();
            robot.Move("left", 60, 180);
            vTaskDelay(pdMS_TO_TICKS(450));
            robot.Move("right", 60, 180);
            vTaskDelay(pdMS_TO_TICKS(450));
            robot.Stop();
            return R{true, false, "xoay thu, xem banh quay"};
        });

        test.AddStep("Wi-Fi robot", []() {
            using R = StartupSelfTest::StepResult;
            auto& ap = RobotAp::GetInstance();
            if (!ap.Enabled()) {
                return R{true, true, "da tat"};
            }
            if (!ap.Active()) {
                return R{false, false, "chua phat"};
            }
            return R{true, false, ap.Ssid()};
        });

        test.AddStep("Web dieu khien", []() {
            using R = StartupSelfTest::StepResult;
            if (!RobotWebServer::GetInstance().running()) {
                return R{false, false, "khong chay"};
            }
            return R{true, false, "cong 8080"};
        });

        test.Start();  // waits for the robot to be ready, then runs once (setting selftest)
    }

    virtual Backlight* GetBacklight() override {
#if EMOTION_DISPLAY_TFT
        static PwmBacklight backlight(TFT_BACKLIGHT_PIN, TFT_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
#else
        return nullptr;
#endif
    }

    // The robot is driven from a web page and a phone: Wi-Fi modem sleep (listen interval ~1 s)
    // makes every button press arrive late, so never let the radio sleep.
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        (void)level;
        WifiBoard::SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(LincRobotV1Board);
