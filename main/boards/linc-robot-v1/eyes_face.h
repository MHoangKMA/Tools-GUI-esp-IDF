#ifndef EYES_FACE_H
#define EYES_FACE_H

#include <functional>
#include <string>

#include <lvgl.h>

// A face with two eyes drawn with plain LVGL objects, shared by the monochrome OLED
// (EyesOledDisplay) and the colour TFT (EyesLcdDisplay).
//
// The shapes are designed on a 128x32 canvas and scaled by `scale_pct` (100 = OLED). On a colour
// screen the palette tints the eyes by mood (angry red, sad blue, sleepy dim) and colours hearts,
// sparkles and tears; on a monochrome screen every palette entry is the same colour.
//
// 27 expressions (plus aliases) chosen by the emotion name, see LookFor():
//   neutral relaxed cool bored confident | happy laughing funny silly loving embarrassed
//   winking kissy delicious excited | sad crying worried scared angry | surprised shocked
//   sleepy sleeping thinking confused dizzy
// The eyes blink every few seconds and glance around while the face is neutral; tears fall while
// crying. Every method must be called with the display (LVGL) lock held, or from the LVGL task.
class EyesFace {
public:
    struct Palette {
        lv_color_t eye;
        lv_color_t bg;
        lv_color_t heart;
        lv_color_t spark;
        lv_color_t tear;
        lv_color_t red;
        lv_color_t blue;
        lv_color_t dim;
    };

    // Creates the stage (`w` x `h` pixels at `x`, `y` inside `parent`) and the idle animation.
    void Build(lv_obj_t* parent, int x, int y, int w, int h, int scale_pct, const Palette& palette);

    // False for an unknown name (the neutral face is shown then).
    bool SetEmotion(const std::string& emotion);

    // Called after every SetEmotion() with the name that was asked for (the board uses it to start
    // a light show). It runs with the display lock held: keep it short.
    using EmotionObserver = std::function<void(const std::string& emotion)>;
    static void SetEmotionObserver(EmotionObserver observer) { emotion_observer_ = std::move(observer); }

private:
    static constexpr int kShapeRects = 7;  // a pixel-art heart needs seven rectangles

    struct Look {
        enum Kind {
            kEyes,       // rounded rectangles
            kArcsUp,     // ^ ^  smiling
            kArcsDown,   // u u  peaceful / asleep
            kWinkRight,  // right eye closed (arch), left eye as described by the fields
            kWinkLeft,   // left eye closed
            kChevrons,   // > <  laughing hard
            kCross,      // X X  dizzy
            kHearts,     // pixel-art hearts
            kStars,      // + + sparkles
        };
        Kind kind = kEyes;
        int w[2] = {28, 28};
        int h[2] = {28, 28};
        int r[2] = {10, 10};
        int lid[2] = {0, 0};    // top lid: 0 none, 1 angry, 2 sad, 3 flat (relaxed), 4 worried
        int cheek[2] = {0, 0};  // lower lid: logical y of its edge, 0 none
        int pupil = 0;          // diameter of a background-coloured pupil, 0 none
        int gaze_x = 0;
        int gaze_y = 0;
        int tint = 0;           // 0 normal, 1 red, 2 blue, 3 dim (colour screens only)
        bool tears = false;
    };

    static Look LookFor(const std::string& emotion, bool* known);
    static void IdleTimerCb(lv_timer_t* timer);
    static void RestoreTimerCb(lv_timer_t* timer);

    int S(int logical) const { return logical * scale_ / 100; }
    int OffsetX(int i) const { return S(i == 0 ? -30 : 30); }
    int CenterX(int i) const { return w_ / 2 + OffsetX(i); }
    int CenterY() const { return h_ / 2; }
    int PxX(int logical_x) const;  // 128x32 canvas x -> stage pixel, clamped to the stage
    int PxY(int logical_y) const;  // 128x32 canvas y -> stage pixel (+ the lid shift)

    void ApplyLook(const Look& look);
    void LayoutEyes();
    void SetTopLid(int i, int type);
    void SetCheekLid(int i, int edge);
    void ShowShape(int i, int kind, lv_color_t color);
    void Blink();
    void IdleTick();
    void SetGaze(int x, int y);

    lv_obj_t* stage_ = nullptr;
    lv_obj_t* eye_[2] = {nullptr, nullptr};
    lv_obj_t* pupil_[2] = {nullptr, nullptr};
    lv_obj_t* arc_[2] = {nullptr, nullptr};
    lv_obj_t* lid_top_[2] = {nullptr, nullptr};
    lv_obj_t* lid_bottom_[2] = {nullptr, nullptr};
    lv_obj_t* poly_[2][2] = {};
    lv_obj_t* shape_[2][kShapeRects] = {};
    lv_obj_t* tear_[2] = {nullptr, nullptr};
    lv_point_precise_t lid_top_points_[2][2] = {};
    lv_point_precise_t lid_bottom_points_[2][2] = {};
    lv_point_precise_t poly_points_[2][2][3] = {};
    static inline EmotionObserver emotion_observer_;
    Palette palette_{};
    Look look_;
    int w_ = 0;
    int h_ = 0;
    int scale_ = 100;
    int lid_shift_ = 0;  // lid lines are drawn with all points >= 0 and the object is moved up
    bool eye_shown_[2] = {true, true};
    int blink_ticks_ = 15;
    int gaze_ticks_ = 35;
    int tear_phase_ = 0;
};

#endif  // EYES_FACE_H
