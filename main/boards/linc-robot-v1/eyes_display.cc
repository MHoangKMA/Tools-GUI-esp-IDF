#include "eyes_display.h"

#include <esp_log.h>

#define TAG "EyesOledDisplay"

namespace {
constexpr int kStageTop = 16;      // below the status bar
constexpr int kStageHeight = 32;
constexpr int kCaptionTop = 48;
}  // namespace

void EyesOledDisplay::SetupUI() {
    OledDisplay::SetupUI();
    if (height_ != 64) {
        return;  // only the 128x64 layout has room for the face
    }

    DisplayLockGuard lock(this);
    auto screen = lv_screen_active();
    // Eyes use the text colour, lids and the cover use the background colour: correct on
    // whichever polarity the monochrome panel uses
    const lv_color_t fg = lv_obj_get_style_text_color(screen, LV_PART_MAIN);
    const lv_color_t bg = lv_obj_get_style_bg_color(screen, LV_PART_MAIN);
    EyesFace::Palette palette{fg, bg, fg, fg, fg, fg, fg, fg};
    face_.Build(screen, 0, kStageTop, width_, kStageHeight, 100, palette);

    // One line of chat text under the eyes
    caption_ = lv_label_create(screen);
    lv_obj_set_size(caption_, width_, LV_SIZE_CONTENT);
    lv_obj_set_pos(caption_, 0, kCaptionTop);
    lv_label_set_long_mode(caption_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(caption_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(caption_, bg, 0);
    lv_obj_set_style_bg_opa(caption_, LV_OPA_COVER, 0);
    lv_label_set_text(caption_, "");
    eyes_ready_ = true;
}

bool EyesOledDisplay::SetEmotion(const char* emotion) {
    if (!eyes_ready_) {
        return OledDisplay::SetEmotion(emotion);  // 128x32 panel
    }
    DisplayLockGuard lock(this);
    return face_.SetEmotion(emotion ? emotion : "neutral");
}

void EyesOledDisplay::SetChatMessage(const char* role, const char* content) {
    if (!eyes_ready_) {
        OledDisplay::SetChatMessage(role, content);
        return;
    }
    DisplayLockGuard lock(this);
    std::string text = content ? content : "";
    for (auto& ch : text) {
        if (ch == '\n') {
            ch = ' ';
        }
    }
    lv_label_set_text(caption_, text.c_str());
}
