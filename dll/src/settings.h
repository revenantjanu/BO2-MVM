// settings.h - settings.ini (plain key=value)
#pragma once
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mvm {

struct Settings {
    // keys are Windows virtual-key codes (decimal)
    int menu_key = 45;          // INSERT   show / hide the menu
    int freecam_key = 70;       // F        freecam on / off (6x: F free camera)
    int frozen_key = 115;       // F4       freeze / unfreeze game time
    int path_add_key = 75;      // K        add keyframe at current camera
    int path_play_key = 71;     // G        dolly camera: play / stop the path
    int path_clear_key = 119;   // F8       clear camera path
    int hud_key = 72;           // H        hide / show points, round, HUD
    int record_key = 120;       // F9       record freecam movement into the path
    int ver = 3;                // settings layout version (<2 = old key defaults, reset on load)
    int roll_left_key = 90;     // Z
    int roll_right_key = 88;    // X

    float ui_scale = 1.0f;
    float move_speed = 260.0f;
    float fast_mult = 3.5f;
    float slow_mult = 0.25f;
    float roll_speed = 60.0f;
    float smooth_move = 9.0f;
    float smooth_look = 20.0f;
    float default_fov = 65.0f;

    bool hide_hud_in_free = true;   // cg_draw2D 0 + cg_drawGun 0 while freecam is on
    bool path_follows_timescale = true;
    float frozen_timescale = 0.0001f;
    int restore_maxfps = 250;   // com_maxfps value restored after capture
    int restore_fov = 90;       // cg_fov restored when freecam turns off

    int capture_fps = 60;
    int capture_format = 0;     // 0 = ffmpeg mp4 (x264), 1 = TGA sequence
    int ffmpeg_crf = 14;
    bool capture_hide_hud = true;
    int smooth_capture = 1;     // fixed-step render: every game frame is one video frame, game time follows the video
    int blur_samples = 1;       // motion blur: sub-frames averaged into each video frame (1 = off)
    int letterbox = 0;          // 0 off, 1 2.39:1, 2 2.00:1, 3 1.85:1
    float vignette = 0.0f;
    float grain = 0.0f;
    float shake = 0.0f;         // handheld camera shake amount
    float shake_speed = 1.0f;
    float exposure = 0.0f;      // stops, -2..2
    float contrast = 0.0f;      // -1..1
    float saturation = 0.0f;    // -1..1
    float temperature = 0.0f;   // -1 cool .. 1 warm
    float tint = 0.0f;          // -1 green .. 1 magenta
    float chroma = 0.0f;        // chromatic aberration, 0..1
    float sharpen = 0.0f;       // 0..1
    int guides = 0;             // 0 off, 1 thirds, 2 center cross, 3 thirds+diagonals
    int ramp_on = 0;            // speed ramp along the camera path
    float ramp_start = 1.0f;
    float ramp_end = 1.0f;
    float bloom = 0.0f;         // glow around bright areas
    float streak = 0.0f;        // horizontal anamorphic light streaks
    float preroll = 0.0f;       // seconds of video recorded before the shot starts
    float rec_smooth = 0.5f;    // free camera recording: jitter removal
    float rec_detail = 0.55f;   // free camera recording: how many keyframes it keeps
    float rec_countdown = 3.0f; // seconds before F9 starts recording
    int demo_freeze_start = 1;  // demos start frozen at their first moment
    int demo_auto_rewind = 0;   // restart the demo after every take
    int path_ease = 0;          // 0 linear, 1 ease in+out, 2 ease in, 3 ease out
    char ffmpeg_path[260] = "ffmpeg.exe";

    bool Load(const std::string& file) {
        FILE* f = std::fopen(file.c_str(), "rb");
        if (!f) return false;
        ver = 0;
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            char* eq = std::strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            std::string k = line, v = eq + 1;
            while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
            Apply(k, v);
        }
        std::fclose(f);
        if (ver < 2) { ver = 2; freecam_key = 70; path_add_key = 75; path_play_key = 71; }
        if (ver < 3) { ver = 3; restore_fov = 90; }          // the game spawns you at 65 (zoomed in): wider by default
        return true;
    }
    bool Save(const std::string& file) const {
        FILE* f = std::fopen(file.c_str(), "wb");
        if (!f) return false;
#define I(n) std::fprintf(f, #n "=%d\n", (int)n);
#define F(n) std::fprintf(f, #n "=%.4f\n", (double)n);
        I(menu_key) I(freecam_key) I(frozen_key) I(path_add_key) I(path_play_key) I(path_clear_key)
        I(roll_left_key) I(roll_right_key) I(hud_key) I(record_key) I(ver)
        F(ui_scale) F(move_speed) F(fast_mult) F(slow_mult) F(roll_speed) F(smooth_move) F(smooth_look) F(default_fov)
        I(hide_hud_in_free) I(path_follows_timescale) F(frozen_timescale) I(restore_maxfps) I(restore_fov)
        I(capture_fps) I(capture_format) I(ffmpeg_crf) I(capture_hide_hud) I(smooth_capture) I(blur_samples) I(letterbox) F(vignette) F(grain) F(shake) F(shake_speed) I(path_ease) F(rec_smooth) F(rec_detail) F(rec_countdown) I(demo_freeze_start) I(demo_auto_rewind) F(bloom) F(streak) F(preroll) F(exposure) F(contrast) F(saturation) F(temperature) F(tint) F(chroma) F(sharpen) I(guides) I(ramp_on) F(ramp_start) F(ramp_end)
#undef I
#undef F
        std::fprintf(f, "ffmpeg_path=%s\n", ffmpeg_path);
        std::fclose(f);
        return true;
    }

private:
    void Apply(const std::string& k, const std::string& v) {
        int iv = std::atoi(v.c_str());
        float fv = (float)std::atof(v.c_str());
#define I(n) if (k == #n) { n = iv; return; }
#define F(n) if (k == #n) { n = fv; return; }
        I(menu_key) I(freecam_key) I(frozen_key) I(path_add_key) I(path_play_key) I(path_clear_key)
        I(roll_left_key) I(roll_right_key) I(hud_key) I(record_key) I(ver)
        F(ui_scale) F(move_speed) F(fast_mult) F(slow_mult) F(roll_speed) F(smooth_move) F(smooth_look) F(default_fov)
        I(hide_hud_in_free) I(path_follows_timescale) F(frozen_timescale) I(restore_maxfps) I(restore_fov)
        I(capture_fps) I(capture_format) I(ffmpeg_crf) I(capture_hide_hud) I(smooth_capture) I(blur_samples) I(letterbox) F(vignette) F(grain) F(shake) F(shake_speed) I(path_ease)
#undef I
#undef F
        if (k == "ffmpeg_path") { std::strncpy(ffmpeg_path, v.c_str(), sizeof ffmpeg_path - 1); ffmpeg_path[sizeof ffmpeg_path - 1] = 0; }
    }
};

}  // namespace mvm
