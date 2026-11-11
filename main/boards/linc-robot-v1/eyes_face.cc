#include "eyes_face.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>

#include <esp_log.h>
#include <esp_random.h>

#define TAG "EyesFace"

namespace {

constexpr int kTickMs = 200;

struct Rect {
    int x, y, w, h;
};
// Pixel-art heart on a 4 px grid (logical units), 28 x 24
const Rect kHeartRects[7] = {{4, 0, 8, 4},  {16, 0, 8, 4}, {0, 4, 28, 4}, {0, 8, 28, 4},
                             {4, 12, 20, 4}, {8, 16, 12, 4}, {12, 20, 4, 4}};
constexpr int kHeartW = 28;
constexpr int kHeartH = 24;
// Sparkle: a plus sign, 26 x 26
const Rect kPlusRects[2] = {{10, 0, 6, 26}, {0, 10, 26, 6}};
constexpr int kPlusSize = 26;

}  // namespace

// ---------------------------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------------------------

EyesFace::Look EyesFace::LookFor(const std::string& emotion, bool* known) {
    Look look;
    *known = true;
    const char* e = emotion.c_str();
    auto is = [e](std::initializer_list<const char*> names) {
        for (const char* name : names) {
            if (strcmp(e, name) == 0) {
                return true;
            }
        }
        return false;
    };
    auto symmetric = [&look](int w, int h, int r) {
        for (int i = 0; i < 2; i++) {
            look.w[i] = w;
            look.h[i] = h;
            look.r[i] = r;
        }
    };

    if (is({"neutral"})) {
        // default round eyes
    } else if (is({"relaxed", "calm"})) {
        symmetric(28, 26, 10);
        look.lid[0] = look.lid[1] = 3;
    } else if (is({"cool"})) {
        symmetric(30, 22, 6);
        look.lid[0] = look.lid[1] = 3;
    } else if (is({"bored", "tired"})) {
        symmetric(30, 16, 5);
        look.lid[0] = look.lid[1] = 3;
        look.tint = 3;
    } else if (is({"confident", "proud"})) {
        look.lid[1] = 1;  // one eyebrow down: a smirk
    } else if (is({"happy", "smile"})) {
        look.kind = Look::kArcsUp;
    } else if (is({"laughing", "joy"})) {
        look.kind = Look::kChevrons;
    } else if (is({"funny"})) {
        look.kind = Look::kWinkLeft;  // left eye shut, right eye big
        look.w[1] = look.h[1] = 32;
        look.r[1] = 16;
    } else if (is({"silly"})) {
        look.w[0] = look.h[0] = 34;
        look.r[0] = 17;
        look.w[1] = look.h[1] = 20;
        look.r[1] = 10;
        look.gaze_x = 4;
    } else if (is({"loving", "heart", "love"})) {
        look.kind = Look::kHearts;
    } else if (is({"embarrassed", "shy"})) {
        look.kind = Look::kArcsUp;
        look.gaze_x = -4;
        look.gaze_y = 4;
    } else if (is({"winking", "wink"})) {
        look.kind = Look::kWinkRight;
    } else if (is({"kissy"})) {
        look.kind = Look::kWinkLeft;
    } else if (is({"delicious", "yummy"})) {
        look.cheek[0] = look.cheek[1] = 22;  // cheeks pushed up
        look.gaze_y = -2;
    } else if (is({"excited"})) {
        look.kind = Look::kStars;
    } else if (is({"sad"})) {
        symmetric(26, 26, 10);
        look.lid[0] = look.lid[1] = 2;
        look.tint = 2;
    } else if (is({"crying", "cry"})) {
        symmetric(26, 26, 10);
        look.lid[0] = look.lid[1] = 2;
        look.tint = 2;
        look.tears = true;
    } else if (is({"worried"})) {
        symmetric(24, 24, 9);
        look.lid[0] = look.lid[1] = 4;
        look.gaze_y = 2;
        look.tint = 2;
    } else if (is({"scared"})) {
        symmetric(30, 30, 15);
        look.lid[0] = look.lid[1] = 4;
        look.pupil = 8;
        look.tint = 2;
    } else if (is({"angry", "mad"})) {
        symmetric(28, 28, 8);
        look.lid[0] = look.lid[1] = 1;
        look.tint = 1;
    } else if (is({"surprised", "wow"})) {
        symmetric(32, 32, 16);
    } else if (is({"shocked"})) {
        symmetric(34, 34, 17);
        look.pupil = 8;
    } else if (is({"sleepy"})) {
        symmetric(30, 6, 3);
        look.tint = 3;
    } else if (is({"sleeping", "sleep"})) {
        look.kind = Look::kArcsDown;
        look.tint = 3;
    } else if (is({"thinking", "think"})) {
        look.w[1] = 24;
        look.h[1] = 20;
        look.r[1] = 8;
        look.lid[1] = 3;
        look.gaze_x = 10;
        look.gaze_y = -6;
    } else if (is({"confused"})) {
        look.w[1] = 20;
        look.h[1] = 20;
        look.r[1] = 8;
        look.gaze_x = -5;
        look.gaze_y = -3;
    } else if (is({"dizzy", "dead"})) {
        look.kind = Look::kCross;
    } else {
        *known = false;
    }
    return look;
}

// ---------------------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------------------

int EyesFace::PxX(int logical_x) const {
    return std::clamp(w_ / 2 + S(logical_x - 64), 0, w_);
}

int EyesFace::PxY(int logical_y) const { return h_ / 2 + S(logical_y - 16) + lid_shift_; }

void EyesFace::Build(lv_obj_t* parent, int x, int y, int w, int h, int scale_pct, const Palette& palette) {
    palette_ = palette;
    w_ = w;
    h_ = h;
    scale_ = scale_pct;
    // Lowest logical y of a lid point is about -10: keep every point >= 0 and move the object up
    lid_shift_ = std::max(0, S(26) - h_ / 2) + 4;

    stage_ = lv_obj_create(parent);
    lv_obj_set_size(stage_, w_, h_);
    lv_obj_set_pos(stage_, x, y);
    lv_obj_set_style_radius(stage_, 0, 0);
    lv_obj_set_style_border_width(stage_, 0, 0);
    lv_obj_set_style_pad_all(stage_, 0, 0);
    lv_obj_set_style_bg_color(stage_, palette_.bg, 0);
    lv_obj_set_style_bg_opa(stage_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(stage_, LV_OBJ_FLAG_SCROLLABLE);

    auto solid = [](lv_obj_t* obj, lv_color_t color) {
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_pad_all(obj, 0, 0);
        lv_obj_set_style_bg_color(obj, color, 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    };
    auto line = [this]() {
        lv_obj_t* l = lv_line_create(stage_);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        return l;
    };
    const int stroke = std::max(3, S(6));

    for (int i = 0; i < 2; i++) {
        eye_[i] = lv_obj_create(stage_);
        solid(eye_[i], palette_.eye);

        pupil_[i] = lv_obj_create(stage_);
        solid(pupil_[i], palette_.bg);
        lv_obj_set_style_radius(pupil_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(pupil_[i], LV_OBJ_FLAG_HIDDEN);

        // Arch used for closed / smiling eyes (angles chosen per expression)
        arc_[i] = lv_arc_create(stage_);
        lv_obj_set_size(arc_[i], S(30), S(30));
        lv_obj_set_style_arc_width(arc_[i], stroke, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc_[i], palette_.eye, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc_[i], 0, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(arc_[i], LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(arc_[i], 0, LV_PART_MAIN);
        lv_obj_remove_style(arc_[i], nullptr, LV_PART_KNOB);
        lv_obj_remove_flag(arc_[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(arc_[i], LV_OBJ_FLAG_HIDDEN);

        // Eyelids: thick lines in the background colour that cut the top / bottom of the eye
        for (lv_obj_t** lid : {&lid_top_[i], &lid_bottom_[i]}) {
            *lid = line();
            lv_obj_set_pos(*lid, 0, -lid_shift_);
            lv_obj_set_style_line_width(*lid, S(16), 0);
            lv_obj_set_style_line_color(*lid, palette_.bg, 0);
            lv_obj_set_style_line_rounded(*lid, false, 0);
        }

        // Strokes for > < and X
        for (int k = 0; k < 2; k++) {
            poly_[i][k] = line();
            lv_obj_set_style_line_width(poly_[i][k], stroke, 0);
            lv_obj_set_style_line_color(poly_[i][k], palette_.eye, 0);
            lv_obj_set_style_line_rounded(poly_[i][k], true, 0);
        }

        // Rectangles for hearts and sparkles
        for (int k = 0; k < kShapeRects; k++) {
            shape_[i][k] = lv_obj_create(stage_);
            solid(shape_[i][k], palette_.eye);
            lv_obj_set_style_radius(shape_[i][k], 0, 0);
            lv_obj_add_flag(shape_[i][k], LV_OBJ_FLAG_HIDDEN);
        }

        // Tear drop
        tear_[i] = lv_obj_create(stage_);
        solid(tear_[i], palette_.tear);
        lv_obj_set_size(tear_[i], std::max(3, S(5)), std::max(4, S(8)));
        lv_obj_set_style_radius(tear_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(tear_[i], LV_OBJ_FLAG_HIDDEN);
    }

    ApplyLook(Look());
    lv_timer_create(IdleTimerCb, kTickMs, this);
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

void EyesFace::SetTopLid(int i, int type) {
    if (type == 0) {
        lv_obj_add_flag(lid_top_[i], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // Left eye coordinates, mirrored for the right eye. The edge of the band that faces the eye
    // goes from the outer to the inner side: angry = inner lower, sad / worried = outer lower.
    const int outer = i == 0 ? 16 : 112;
    const int inner = i == 0 ? 52 : 76;
    int y_outer = 0;
    int y_inner = 0;
    int x_from = outer;
    int x_to = inner;
    switch (type) {
        case 1: y_outer = -6; y_inner = 6; break;    // angry
        case 2: y_outer = 4; y_inner = -8; break;    // sad
        case 4: y_outer = 8; y_inner = -10; break;   // worried
        default:                                      // 3: flat lid
            y_outer = y_inner = 3;
            x_from = i == 0 ? 8 : 120;
            x_to = i == 0 ? 56 : 72;
            break;
    }
    lid_top_points_[i][0] = {PxX(x_from), PxY(y_outer)};
    lid_top_points_[i][1] = {PxX(x_to), PxY(y_inner)};
    lv_line_set_points(lid_top_[i], lid_top_points_[i], 2);
    lv_obj_remove_flag(lid_top_[i], LV_OBJ_FLAG_HIDDEN);
}

void EyesFace::SetCheekLid(int i, int edge) {
    if (edge <= 0) {
        lv_obj_add_flag(lid_bottom_[i], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const int y = PxY(edge + 8);  // the band covers everything below `edge`
    lid_bottom_points_[i][0] = {std::max(0, CenterX(i) - S(22)), y};
    lid_bottom_points_[i][1] = {std::min(w_, CenterX(i) + S(22)), y};
    lv_line_set_points(lid_bottom_[i], lid_bottom_points_[i], 2);
    lv_obj_remove_flag(lid_bottom_[i], LV_OBJ_FLAG_HIDDEN);
}

void EyesFace::ShowShape(int i, int kind, lv_color_t color) {
    const int cx = CenterX(i);
    const int cy = CenterY();
    const bool heart = kind == Look::kHearts;
    const Rect* rects = heart ? kHeartRects : kPlusRects;
    const int count = heart ? kShapeRects : 2;
    const int box_w = heart ? kHeartW : kPlusSize;
    const int box_h = heart ? kHeartH : kPlusSize;
    for (int k = 0; k < count; k++) {
        lv_obj_set_style_bg_color(shape_[i][k], color, 0);
        lv_obj_set_size(shape_[i][k], S(rects[k].w), S(rects[k].h));
        lv_obj_set_pos(shape_[i][k], cx - S(box_w) / 2 + S(rects[k].x), cy - S(box_h) / 2 + S(rects[k].y));
        lv_obj_remove_flag(shape_[i][k], LV_OBJ_FLAG_HIDDEN);
    }
}

// Sizes and positions of the round eyes and their pupils (also restores them after a blink)
void EyesFace::LayoutEyes() {
    for (int i = 0; i < 2; i++) {
        if (!eye_shown_[i]) {
            continue;
        }
        lv_obj_set_size(eye_[i], S(look_.w[i]), S(look_.h[i]));
        lv_obj_align(eye_[i], LV_ALIGN_CENTER, OffsetX(i), 0);
        if (look_.pupil > 0) {
            lv_obj_set_size(pupil_[i], S(look_.pupil), S(look_.pupil));
            lv_obj_align(pupil_[i], LV_ALIGN_CENTER, OffsetX(i) + S(look_.gaze_x), S(look_.gaze_y));
            lv_obj_remove_flag(pupil_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void EyesFace::ApplyLook(const Look& look) {
    look_ = look;
    tear_phase_ = 0;
    const lv_color_t eye_color = look.tint == 1   ? palette_.red
                                 : look.tint == 2 ? palette_.blue
                                 : look.tint == 3 ? palette_.dim
                                                  : palette_.eye;
    for (int i = 0; i < 2; i++) {
        // Start from a clean eye: hide every optional part
        lv_obj_add_flag(eye_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pupil_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(arc_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lid_top_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lid_bottom_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tear_[i], LV_OBJ_FLAG_HIDDEN);
        for (int k = 0; k < 2; k++) {
            lv_obj_add_flag(poly_[i][k], LV_OBJ_FLAG_HIDDEN);
        }
        for (int k = 0; k < kShapeRects; k++) {
            lv_obj_add_flag(shape_[i][k], LV_OBJ_FLAG_HIDDEN);
        }
        eye_shown_[i] = false;

        const bool arch = look.kind == Look::kArcsUp || look.kind == Look::kArcsDown ||
                          (look.kind == Look::kWinkRight && i == 1) || (look.kind == Look::kWinkLeft && i == 0);
        const int cx = CenterX(i);
        const int cy = CenterY();
        const int nine = S(9);
        const int seven = S(7);
        if (arch) {
            const bool down = look.kind == Look::kArcsDown;
            lv_arc_set_bg_angles(arc_[i], down ? 25 : 205, down ? 155 : 335);  // bowl / arch
            lv_obj_set_style_arc_color(arc_[i], eye_color, LV_PART_MAIN);
            lv_obj_align(arc_[i], LV_ALIGN_CENTER, OffsetX(i) + S(look.gaze_x),
                         (down ? -S(6) : S(6)) + S(look.gaze_y));
            lv_obj_remove_flag(arc_[i], LV_OBJ_FLAG_HIDDEN);
        } else if (look.kind == Look::kChevrons) {
            // > on the left, < on the right, pointing at the nose
            const int sign = i == 0 ? 1 : -1;
            poly_points_[i][0][0] = {cx - sign * nine, cy - nine};
            poly_points_[i][0][1] = {cx + sign * seven, cy};
            poly_points_[i][0][2] = {cx - sign * nine, cy + nine};
            lv_obj_set_style_line_color(poly_[i][0], eye_color, 0);
            lv_line_set_points(poly_[i][0], poly_points_[i][0], 3);
            lv_obj_remove_flag(poly_[i][0], LV_OBJ_FLAG_HIDDEN);
        } else if (look.kind == Look::kCross) {
            poly_points_[i][0][0] = {cx - nine, cy - nine};
            poly_points_[i][0][1] = {cx + nine, cy + nine};
            poly_points_[i][1][0] = {cx + nine, cy - nine};
            poly_points_[i][1][1] = {cx - nine, cy + nine};
            for (int k = 0; k < 2; k++) {
                lv_obj_set_style_line_color(poly_[i][k], eye_color, 0);
                lv_line_set_points(poly_[i][k], poly_points_[i][k], 2);
                lv_obj_remove_flag(poly_[i][k], LV_OBJ_FLAG_HIDDEN);
            }
        } else if (look.kind == Look::kHearts || look.kind == Look::kStars) {
            ShowShape(i, look.kind, look.kind == Look::kHearts ? palette_.heart : palette_.spark);
        } else {
            eye_shown_[i] = true;
            lv_obj_set_style_bg_color(eye_[i], eye_color, 0);
            lv_obj_remove_flag(eye_[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_radius(eye_[i], S(look.r[i]), 0);
            lv_obj_set_style_translate_x(eye_[i], S(look.gaze_x), 0);
            lv_obj_set_style_translate_y(eye_[i], S(look.gaze_y), 0);
            SetTopLid(i, look.lid[i]);
            SetCheekLid(i, look.cheek[i]);
        }
        if (look.tears) {
            lv_obj_remove_flag(tear_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    LayoutEyes();
}

bool EyesFace::SetEmotion(const std::string& emotion) {
    bool known = false;
    ApplyLook(LookFor(emotion, &known));
    if (emotion_observer_) {
        emotion_observer_(emotion);
    }
    return known;
}

// ---------------------------------------------------------------------------------------------
// Life: blinking, glancing, tears (all run in the LVGL task)
// ---------------------------------------------------------------------------------------------

void EyesFace::SetGaze(int x, int y) {
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_translate_x(eye_[i], S(x), 0);
        lv_obj_set_style_translate_y(eye_[i], S(y), 0);
    }
}

void EyesFace::Blink() {
    for (int i = 0; i < 2; i++) {
        if (!eye_shown_[i]) {
            continue;
        }
        lv_obj_set_size(eye_[i], S(look_.w[i]), std::max(2, S(4)));
        lv_obj_align(eye_[i], LV_ALIGN_CENTER, OffsetX(i), 0);
        lv_obj_add_flag(pupil_[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_t* restore = lv_timer_create(RestoreTimerCb, 130, this);
    lv_timer_set_repeat_count(restore, 1);  // one shot, deleted by LVGL afterwards
}

void EyesFace::IdleTick() {
    const bool round_eyes = look_.kind == Look::kEyes && look_.h[0] > 10;
    if (--blink_ticks_ <= 0) {
        blink_ticks_ = 15 + static_cast<int>(esp_random() % 16);  // every 3 - 6 s
        if (round_eyes) {
            Blink();
        }
    }
    if (--gaze_ticks_ <= 0) {
        gaze_ticks_ = 30 + static_cast<int>(esp_random() % 20);  // every 6 - 10 s
        // Glance around only while the face is plain neutral
        if (round_eyes && look_.lid[0] == 0 && look_.lid[1] == 0 && look_.cheek[0] == 0 &&
            look_.gaze_x == 0 && look_.gaze_y == 0 && look_.pupil == 0) {
            static const int xs[] = {-8, 0, 8, 0};
            static const int ys[] = {0, -3, 3, 0};
            SetGaze(xs[esp_random() % 4], ys[esp_random() % 4]);
        }
    }
    if (look_.tears) {
        // A tear runs down beside each eye, about 1 s per drop
        tear_phase_ = (tear_phase_ + 1) % 6;
        for (int i = 0; i < 2; i++) {
            const int side = S(look_.w[i] / 2 + 5) * (i == 0 ? -1 : 1);
            lv_obj_align(tear_[i], LV_ALIGN_CENTER, OffsetX(i) + side, S(-8 + tear_phase_ * 4));
        }
    }
}

void EyesFace::IdleTimerCb(lv_timer_t* timer) {
    static_cast<EyesFace*>(lv_timer_get_user_data(timer))->IdleTick();
}

void EyesFace::RestoreTimerCb(lv_timer_t* timer) {
    static_cast<EyesFace*>(lv_timer_get_user_data(timer))->LayoutEyes();
}
