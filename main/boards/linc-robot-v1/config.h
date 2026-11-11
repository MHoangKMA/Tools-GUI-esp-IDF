#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// Robot board: ESP32-S3 N16R8 + INMP441 + MAX98357A + DRV8833 + two OLEDs + two LEDs.
//
// Given by the owner: LEDs (GPIO46 / GPIO48), DRV8833 pins, one I2C bus for both OLEDs (SDA41 / SCL42).
// ASSUMED (not given, standard Xiaozhi breadboard wiring): INMP441 / MAX98357A / buttons.
// Edit the assumed values when your wiring differs.

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define TOUCH_BUTTON_GPIO       GPIO_NUM_47
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_40
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_39

// Two LEDs used as turn signals (TurnSignals): the LEFT one blinks while the robot turns left, the
// RIGHT one while it turns right, both while it drives straight forward or backward. Swap the two
// pins if the lights sit on the other sides. GPIO46 is a strapping pin (only read at reset), which
// is fine for an LED. SIGNAL_ACTIVE_LOW = true when an LED lights with the pin LOW.
#define LEFT_SIGNAL_GPIO  GPIO_NUM_46
#define RIGHT_SIGNAL_GPIO GPIO_NUM_48
#define SIGNAL_ACTIVE_LOW false
// Light shows that go with faces (angry = fast flashes, happy = alternating ...). 0 = the lights
// only follow the wheels and the commands.
#define LIGHT_EFFECTS_ENABLED 1

// Both OLEDs share ONE I2C bus (SDA41 / SCL42), so they must have different addresses.
// Two modules of the same kind usually both answer at 0x3C: move one of them to 0x3D with the
// address jumper / solder pad on its back, then make the two macros below match. The boot log
// lists every address found on the bus. If the two screens show each other's content, swap
// the two addresses.
#define OLED_I2C_SDA_PIN GPIO_NUM_41
#define OLED_I2C_SCL_PIN GPIO_NUM_42

// Song OLED: 0.96" SSD1306 128x64 (song title + spectrum). Own driver (song_display.cc).
#define SONG_OLED_ADDR     0x3C
#define SONG_OLED_MIRROR_X true
#define SONG_OLED_MIRROR_Y true

// Emotion screen: which display shows the two-eye face. 1 = the 1.3" colour TFT below (default),
// 0 = a 1.3" monochrome OLED on the shared I2C bus (the EMOTION_OLED_* values further down).
#define EMOTION_DISPLAY_TFT 1

// Emotion TFT: 1.3" IPS ST7789, 240x240, SPI, 7-pin module (GND, VCC, SCL, SDA, RES, DC, BLK).
// This module has NO chip-select pin (CS is wired low on the module), hence SPI mode 3 and
// CS = NC. Pins as wired by the owner:
//   SCL -> GPIO9   SDA (MOSI) -> GPIO10   RES -> GPIO18   DC -> GPIO8   BLK -> GPIO17
#define TFT_WIDTH   240
#define TFT_HEIGHT  240
#define TFT_CLK_PIN  GPIO_NUM_9
#define TFT_MOSI_PIN GPIO_NUM_10
#define TFT_CS_PIN   GPIO_NUM_NC
#define TFT_DC_PIN   GPIO_NUM_8
#define TFT_RST_PIN  GPIO_NUM_18
#define TFT_BACKLIGHT_PIN GPIO_NUM_17
#define TFT_BACKLIGHT_OUTPUT_INVERT false
#define TFT_SPI_MODE 3
#define TFT_PCLK_HZ  (40 * 1000 * 1000)
#define TFT_MIRROR_X false
#define TFT_MIRROR_Y false
#define TFT_SWAP_XY  false
#define TFT_INVERT_COLOR true   // IPS ST7789 modules are wired inverted
#define TFT_OFFSET_X 0
#define TFT_OFFSET_Y 0

// Emotion OLED (only when EMOTION_DISPLAY_TFT is 0): 1.3" 128x64 on the shared I2C bus, used
// through LVGL.
// 1.3" modules are almost always SH1106; set EMOTION_OLED_IS_SH1106 to 0 for an SSD1306.
#define EMOTION_OLED_ADDR      0x3D
#define EMOTION_OLED_IS_SH1106 1
#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  64
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

// DRV8833 motor driver. The chip has no enable pins: speed is PWM on the IN pins.
// Forward = PWM on IN1 with IN2 low, reverse = PWM on IN2 with IN1 low.
// Swap the pins or set *_INVERT when a wheel turns the wrong way.
#define MOTOR_A_IN1_GPIO GPIO_NUM_12
#define MOTOR_A_IN2_GPIO GPIO_NUM_13
#define MOTOR_B_IN1_GPIO GPIO_NUM_14
#define MOTOR_B_IN2_GPIO GPIO_NUM_21
#define MOTOR_LEFT_INVERT  false   // channel A = left wheel
#define MOTOR_RIGHT_INVERT false   // channel B = right wheel

#endif // _BOARD_CONFIG_H_
