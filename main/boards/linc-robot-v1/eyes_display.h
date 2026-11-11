#ifndef EYES_DISPLAY_H
#define EYES_DISPLAY_H

#include "display/oled_display.h"
#include "eyes_face.h"

// Monochrome OLED (128x64) that shows the robot as a face with two eyes (EyesFace) instead of
// an emoji:
//
//   rows  0-15  status bar (network, notifications), unchanged OledDisplay
//   rows 16-47  the two eyes
//   rows 48-63  one line of the latest chat text (scrolls when long)
//
// Only the 128x64 layout has the face; with a 128x32 panel this class behaves like OledDisplay.
class EyesOledDisplay : public OledDisplay {
public:
    using OledDisplay::OledDisplay;

    void SetupUI() override;
    bool SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;

private:
    EyesFace face_;
    lv_obj_t* caption_ = nullptr;
    bool eyes_ready_ = false;
};

#endif  // EYES_DISPLAY_H
