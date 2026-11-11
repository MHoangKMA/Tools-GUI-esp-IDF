#ifndef EYES_LCD_DISPLAY_H
#define EYES_LCD_DISPLAY_H

#include "display/lcd_display.h"
#include "eyes_face.h"

// Colour TFT (ST7789 240x240) that shows the robot as a face with two eyes (EyesFace):
//
//   top bar   network / mute / battery icons and notifications (the stock LcdDisplay bar)
//   middle    the two eyes, tinted by mood: angry red, sad blue, sleepy dim, hearts pink, ...
//   bottom    one line of the latest chat text (scrolls when long)
//
// The face covers the stock emoji / chat area, which stays untouched underneath.
class EyesLcdDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;

    void SetupUI() override;
    bool SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;

private:
    EyesFace face_;
    lv_obj_t* cover_ = nullptr;
    lv_obj_t* caption_ = nullptr;
    bool eyes_ready_ = false;
};

#endif  // EYES_LCD_DISPLAY_H
