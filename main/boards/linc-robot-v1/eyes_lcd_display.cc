#include "eyes_lcd_display.h"

#include <esp_log.h>

#include "lvgl_theme.h"

#define TAG "EyesLcdDisplay"

namespace {
constexpr int kFallbackTopBar = 36;
constexpr int kStageHeight = 110;   // pixels: the 128x32 design scaled 2.2x is 70 px tall, with margin
constexpr int kScalePercent = 220;  // 128 logical px -> 282 px, the face itself spans ~220 px
constexpr int kCaptionGap = 10;
}  // namespace

void EyesLcdDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();

    DisplayLockGuard lock(this);
    // The face is drawn on black; the stock top bar should match
    if (LvglTheme* dark = LvglThemeManager::GetInstance().GetTheme("dark")) {
        SetTheme(dark);
    }

    auto screen = lv_screen_active();
    int top = kFallbackTopBar;
    if (top_bar_ != nullptr) {
        lv_obj_update_layout(top_bar_);
        top = lv_obj_get_height(top_bar_);
    }
    const int cover_height = height_ - top;

    cover_ = lv_obj_create(screen);
    lv_obj_set_size(cover_, width_, cover_height);
    lv_obj_set_pos(cover_, 0, top);
    lv_obj_set_style_radius(cover_, 0, 0);
    lv_obj_set_style_border_width(cover_, 0, 0);
    lv_obj_set_style_pad_all(cover_, 0, 0);
    lv_obj_set_style_bg_color(cover_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cover_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cover_, LV_OBJ_FLAG_SCROLLABLE);

    EyesFace::Palette palette{
        .eye = lv_color_hex(0x35D0FF),    // cyan
        .bg = lv_color_black(),
        .heart = lv_color_hex(0xFF3B6B),  // pink-red
        .spark = lv_color_hex(0xFFD23B),  // yellow
        .tear = lv_color_hex(0x5AB4FF),   // light blue
        .red = lv_color_hex(0xFF5030),
        .blue = lv_color_hex(0x4D7BFF),
        .dim = lv_color_hex(0x2B7C96),
    };
    const int stage_y = (cover_height - kStageHeight - 40) / 2;
    face_.Build(cover_, 0, stage_y, width_, kStageHeight, kScalePercent, palette);

    // One line of chat text under the eyes
    caption_ = lv_label_create(cover_);
    lv_obj_set_width(caption_, width_ - 16);
    lv_obj_set_pos(caption_, 8, stage_y + kStageHeight + kCaptionGap);
    lv_label_set_long_mode(caption_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(caption_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(caption_, lv_color_white(), 0);
    if (auto* theme = static_cast<LvglTheme*>(current_theme_)) {
        lv_obj_set_style_text_font(caption_, theme->text_font()->font(), 0);
    }
    lv_label_set_text(caption_, "");
    eyes_ready_ = true;
}

bool EyesLcdDisplay::SetEmotion(const char* emotion) {
    if (!eyes_ready_) {
        return SpiLcdDisplay::SetEmotion(emotion);
    }
    DisplayLockGuard lock(this);
    return face_.SetEmotion(emotion ? emotion : "neutral");
}

void EyesLcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!eyes_ready_) {
        SpiLcdDisplay::SetChatMessage(role, content);
        return;
    }
    DisplayLockGuard lock(this);
    std::string text = content ? content : "";
    for (auto& ch : text) {
        if (ch == '\n') {
            ch = ' ';
        }
    }
    // The theme (and its fonts) can be replaced after the UI is built (assets refresh),
    // so re-apply the current font instead of trusting the pointer taken at build time.
    if (auto* theme = static_cast<LvglTheme*>(current_theme_)) {
        if (theme->text_font() != nullptr && theme->text_font()->font() != nullptr) {
            lv_obj_set_style_text_font(caption_, theme->text_font()->font(), 0);
        }
    }
    lv_label_set_text(caption_, text.c_str());
}
