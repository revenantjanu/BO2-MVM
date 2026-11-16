// camera.h - platform independent camera math, freecam state and camera path.
// No Windows / game dependencies on purpose, so it can be unit tested anywhere.
#pragma once
#include <vector>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace mvm {

struct Vec3 { float x = 0, y = 0, z = 0; };

static inline float Deg2Rad(float d) { return d * 0.017453292519943295f; }
static inline float Rad2Deg(float r) { return r * 57.29577951308232f; }

static inline float NormAngle180(float a) {
    a = std::fmod(a + 180.0f, 360.0f);
    if (a < 0) a += 360.0f;
    return a - 180.0f;
}
// shortest-path angle lerp
static inline float LerpAngle(float a, float b, float t) {
    return a + NormAngle180(b - a) * t;
}
static inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// Angles use engine convention: pitch (+ looks down), yaw (+ counter clockwise), roll.
struct CamPose {
    Vec3 pos;
    float pitch = 0, yaw = 0, roll = 0;
    float fov = 65.0f;
};

// Forward/right/up from engine angles (used for freecam movement).
static inline void AngleVectors(float pitch, float yaw, float roll, Vec3* f, Vec3* r, Vec3* u) {
    float sp = std::sin(Deg2Rad(pitch)), cp = std::cos(Deg2Rad(pitch));
    float sy = std::sin(Deg2Rad(yaw)),   cy = std::cos(Deg2Rad(yaw));
    float sr = std::sin(Deg2Rad(roll)),  cr = std::cos(Deg2Rad(roll));
    if (f) { f->x = cp * cy; f->y = cp * sy; f->z = -sp; }
    if (r) { r->x = -sr * sp * cy + cr * sy; r->y = -sr * sp * sy - cr * cy; r->z = -sr * cp; }
    if (u) { u->x = cr * sp * cy + sr * sy;  u->y = cr * sp * sy - sr * cy;  u->z = cr * cp; }
}

// Pitch/yaw from a forward vector (roll cannot be recovered, returned as 0).
static inline void ForwardToAngles(const Vec3& f, float* pitch, float* yaw) {
    float h = std::sqrt(f.x * f.x + f.y * f.y);
    *pitch = Rad2Deg(std::atan2(-f.z, h));
    *yaw = (h < 1e-6f && std::fabs(f.z) > 0) ? 0.0f : Rad2Deg(std::atan2(f.y, f.x));
}

// ------------------------------------------------------------------ path ---
struct CamKey {
    double timeMs = 0;   // absolute time on the timeline
    CamPose pose;
    int ease = 0;        // how the segment that STARTS at this key is timed: 0 none, 1 in+out, 2 in, 3 out, 4 hold
    float speed = 1.0f;  // game speed at this key (slow motion / fast), blended between keys
    float tan = 1.0f;    // handle length of the curve at this key (0 = sharp corner, 1 = smooth, 2 = wide swing)
};

static inline float EaseT(int e, float t) {
    switch (e) {
        case 1: return t * t * (3 - 2 * t);
        case 2: return t * t;
        case 3: return 1 - (1 - t) * (1 - t);
        case 4: return 0.0f;
    }
    return t;
}

enum class Interp { Linear = 0, Smooth = 1 };

struct CamPath {
    std::vector<CamKey> keys;
    Interp interp = Interp::Smooth;

    void Sort() {
        std::sort(keys.begin(), keys.end(), [](const CamKey& a, const CamKey& b) { return a.timeMs < b.timeMs; });
    }
    double Duration() const { return keys.empty() ? 0.0 : keys.back().timeMs; }

    static CamPose Mix(const CamPose& a, const CamPose& b, float t) {
        CamPose o;
        o.pos.x = Lerp(a.pos.x, b.pos.x, t); o.pos.y = Lerp(a.pos.y, b.pos.y, t); o.pos.z = Lerp(a.pos.z, b.pos.z, t);
        o.pitch = LerpAngle(a.pitch, b.pitch, t); o.yaw = LerpAngle(a.yaw, b.yaw, t); o.roll = LerpAngle(a.roll, b.roll, t);
        o.fov = Lerp(a.fov, b.fov, t);
        return o;
    }

    // Catmull-Rom on a scalar. p1->p2 segment, t in [0,1].
    static float CR(float p0, float p1, float p2, float p3, float t) {
        float t2 = t * t, t3 = t2 * t;
        return 0.5f * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
    }
    // Angles are first unwrapped relative to p1 so the spline takes the short way.
    static float CRAng(float a0, float a1, float a2, float a3, float t) {
        float d0 = NormAngle180(a0 - a1), d2 = NormAngle180(a2 - a1), d3 = d2 + NormAngle180(a3 - a2);
        return a1 + CR(d0, 0.0f, d2, d3, t);
    }
    // Hermite with adjustable handle length at both ends (tn = 1 is exactly Catmull-Rom)
    static float Herm(float p0, float p1, float p2, float p3, float t, float tn1, float tn2) {
        float m1 = 0.5f * (p2 - p0) * tn1, m2 = 0.5f * (p3 - p1) * tn2;
        float t2 = t * t, t3 = t2 * t;
        return (2 * t3 - 3 * t2 + 1) * p1 + (t3 - 2 * t2 + t) * m1 + (-2 * t3 + 3 * t2) * p2 + (t3 - t2) * m2;
    }
    static float HermAng(float a0, float a1, float a2, float a3, float t, float tn1, float tn2) {
        float d0 = NormAngle180(a0 - a1), d2 = NormAngle180(a2 - a1), d3 = d2 + NormAngle180(a3 - a2);
        return a1 + Herm(d0, 0.0f, d2, d3, t, tn1, tn2);
    }
    bool HasSpeed() const { for (const auto& k : keys) if (std::fabs(k.speed - 1.0f) > 0.001f) return true; return false; }
    float SampleSpeed(double tMs) const {
        if (keys.empty()) return 1.0f;
        if (keys.size() == 1 || tMs <= keys.front().timeMs) return keys.front().speed;
        if (tMs >= keys.back().timeMs) return keys.back().speed;
        size_t i = 0;
        while (i + 1 < keys.size() && keys[i + 1].timeMs <= tMs) ++i;
        double span = keys[i + 1].timeMs - keys[i].timeMs;
        float t = span > 0 ? (float)((tMs - keys[i].timeMs) / span) : 0.0f;
        t = t * t * (3 - 2 * t);
        return Lerp(keys[i].speed, keys[i + 1].speed, t);
    }

    bool Sample(double tMs, CamPose* out) const {
        if (keys.empty()) return false;
        if (keys.size() == 1 || tMs <= keys.front().timeMs) { *out = keys.front().pose; return true; }
        if (tMs >= keys.back().timeMs) { *out = keys.back().pose; return true; }
        size_t i = 0;
        while (i + 1 < keys.size() && keys[i + 1].timeMs <= tMs) ++i;
        const CamKey& k1 = keys[i];
        const CamKey& k2 = keys[i + 1];
        double span = k2.timeMs - k1.timeMs;
        float t = span > 0 ? (float)((tMs - k1.timeMs) / span) : 0.0f;
        t = EaseT(k1.ease, t);
        if (interp == Interp::Linear || keys.size() < 3) { *out = Mix(k1.pose, k2.pose, t); return true; }
        const CamKey& k0 = keys[i > 0 ? i - 1 : i];
        const CamKey& k3 = keys[i + 2 < keys.size() ? i + 2 : i + 1];
        CamPose o;
        o.pos.x = Herm(k0.pose.pos.x, k1.pose.pos.x, k2.pose.pos.x, k3.pose.pos.x, t, k1.tan, k2.tan);
        o.pos.y = Herm(k0.pose.pos.y, k1.pose.pos.y, k2.pose.pos.y, k3.pose.pos.y, t, k1.tan, k2.tan);
        o.pos.z = Herm(k0.pose.pos.z, k1.pose.pos.z, k2.pose.pos.z, k3.pose.pos.z, t, k1.tan, k2.tan);
        o.pitch = HermAng(k0.pose.pitch, k1.pose.pitch, k2.pose.pitch, k3.pose.pitch, t, k1.tan, k2.tan);
        o.yaw = HermAng(k0.pose.yaw, k1.pose.yaw, k2.pose.yaw, k3.pose.yaw, t, k1.tan, k2.tan);
        o.roll = HermAng(k0.pose.roll, k1.pose.roll, k2.pose.roll, k3.pose.roll, t, k1.tan, k2.tan);
        o.fov   = Herm(k0.pose.fov, k1.pose.fov, k2.pose.fov, k3.pose.fov, t, k1.tan, k2.tan);
        *out = o;
        return true;
    }

    // ---- editing helpers (recorded takes, trimming, retiming)
    static CamPose PoseAt(const std::vector<CamKey>& ks, double t) { CamPath p; p.keys = ks; p.interp = Interp::Linear; CamPose o; p.Sample(t, &o); return o; }
    // zero-phase low-pass of a dense recording: removes hand jitter without delaying the shot. amount 0..1
    static std::vector<CamKey> Smooth(const std::vector<CamKey>& in, float amount) {
        if (in.size() < 5 || amount <= 0.001f) return in;
        int rad = std::max(1, (int)(amount * 14.0f));                  // samples are ~33 ms apart
        std::vector<CamKey> o = in;
        for (size_t i = 0; i < in.size(); ++i) {
            double sw = 0, x = 0, y = 0, z = 0, f = 0, dp = 0, dy = 0, dr = 0;
            for (int d = -rad; d <= rad; ++d) {
                long j = (long)i + d;
                if (j < 0 || j >= (long)in.size()) continue;
                double w = 1.0 - std::fabs((double)d) / (rad + 1);
                sw += w; x += in[j].pose.pos.x * w; y += in[j].pose.pos.y * w; z += in[j].pose.pos.z * w; f += in[j].pose.fov * w;
                dp += NormAngle180(in[j].pose.pitch - in[i].pose.pitch) * w;
                dy += NormAngle180(in[j].pose.yaw - in[i].pose.yaw) * w;
                dr += NormAngle180(in[j].pose.roll - in[i].pose.roll) * w;
            }
            if (sw <= 0) continue;
            o[i].pose.pos.x = (float)(x / sw); o[i].pose.pos.y = (float)(y / sw); o[i].pose.pos.z = (float)(z / sw); o[i].pose.fov = (float)(f / sw);
            o[i].pose.pitch = NormAngle180(in[i].pose.pitch + (float)(dp / sw));
            o[i].pose.yaw = NormAngle180(in[i].pose.yaw + (float)(dy / sw));
            o[i].pose.roll = NormAngle180(in[i].pose.roll + (float)(dr / sw));
        }
        o.front() = in.front(); o.back() = in.back();               // the first and last frames stay exactly where you put them
        return o;
    }
    // reduce a dense recording to a few editable keyframes that still follow it closely. detail 0 = fewest keys .. 1 = most
    static std::vector<CamKey> Simplify(const std::vector<CamKey>& in, float detail) {
        if (in.size() <= 3) return in;
        float k = 1.0f + (1.0f - std::max(0.0f, std::min(1.0f, detail))) * 9.0f;     // tolerance multiplier
        float posTol = 3.0f * k, angTol = 0.8f * k, fovTol = 1.0f * k;
        std::vector<char> keep(in.size(), 0);
        keep.front() = keep.back() = 1;
        std::vector<std::pair<size_t, size_t>> st;
        st.push_back({0, in.size() - 1});
        while (!st.empty()) {
            auto seg = st.back(); st.pop_back();
            if (seg.second <= seg.first + 1) continue;
            double t0 = in[seg.first].timeMs, t1 = in[seg.second].timeMs;
            float worst = 0; size_t wi = 0;
            for (size_t i = seg.first + 1; i < seg.second; ++i) {
                float t = t1 > t0 ? (float)((in[i].timeMs - t0) / (t1 - t0)) : 0;
                CamPose m = Mix(in[seg.first].pose, in[seg.second].pose, t);
                const CamPose& p = in[i].pose;
                float dp = std::sqrt((p.pos.x - m.pos.x) * (p.pos.x - m.pos.x) + (p.pos.y - m.pos.y) * (p.pos.y - m.pos.y) + (p.pos.z - m.pos.z) * (p.pos.z - m.pos.z)) / posTol;
                float da = std::max(std::fabs(NormAngle180(p.pitch - m.pitch)), std::max(std::fabs(NormAngle180(p.yaw - m.yaw)), std::fabs(NormAngle180(p.roll - m.roll)))) / angTol;
                float df = std::fabs(p.fov - m.fov) / fovTol;
                float e = std::max(dp, std::max(da, df));
                if (e > worst) { worst = e; wi = i; }
            }
            if (worst > 1.0f) { keep[wi] = 1; st.push_back({seg.first, wi}); st.push_back({wi, seg.second}); }
        }
        std::vector<CamKey> o;
        for (size_t i = 0; i < in.size(); ++i) if (keep[i]) o.push_back(in[i]);
        return o;
    }
    void Trim(double fromMs, double toMs) {                            // keep only [from, to], moved so it starts at 0
        if (keys.size() < 2 || toMs - fromMs < 50) return;
        CamKey a, b; a.pose = PoseAt(keys, fromMs); b.pose = PoseAt(keys, toMs);
        std::vector<CamKey> o;
        bool smooth = interp == Interp::Smooth;
        CamPath tmp; tmp.keys = keys; tmp.interp = interp;
        CamPose pa, pb; tmp.Sample(fromMs, &pa); tmp.Sample(toMs, &pb);
        a.pose = pa; a.timeMs = 0; o.push_back(a);
        for (auto& k : keys) if (k.timeMs > fromMs + 20 && k.timeMs < toMs - 20) { CamKey n = k; n.timeMs -= fromMs; o.push_back(n); }
        b.pose = pb; b.timeMs = toMs - fromMs; o.push_back(b);
        (void)smooth;
        keys = o;
    }
    void StretchTo(double totalMs) {
        double d = Duration();
        if (d <= 0 || totalMs < 100) return;
        double f = totalMs / d;
        for (auto& k : keys) k.timeMs *= f;
    }
    void Reverse() {
        double d = Duration();
        std::reverse(keys.begin(), keys.end());
        for (auto& k : keys) k.timeMs = d - k.timeMs;
        for (size_t i = 0; i + 1 < keys.size(); ++i) {                 // an ease belongs to the segment that starts at a key, so it moves with the direction
            int e = keys[i].ease; (void)e;
        }
    }
    void EvenOut() {                                                    // same time between every point, total length unchanged
        if (keys.size() < 3) return;
        double d = Duration();
        for (size_t i = 0; i < keys.size(); ++i) keys[i].timeMs = d * (double)i / (double)(keys.size() - 1);
    }
    double Checksum() const {
        double s = (double)keys.size() * 1000.0;
        for (const auto& k : keys) s += k.timeMs * 0.001 + k.pose.pos.x * 0.01 + k.pose.pos.y * 0.013 + k.pose.pos.z * 0.017 + k.pose.yaw * 0.1 + k.pose.pitch * 0.07 + k.pose.fov * 0.3 + k.ease * 11 + k.speed * 5 + k.tan * 3;
        return s;
    }

    // File format (plain text):
    //   bo2campath 1
    //   interp 0|1
    //   cam time_ms x y z pitch yaw roll fov
    bool Save(const std::string& file) const {
        FILE* f = std::fopen(file.c_str(), "wb");
        if (!f) return false;
        std::fprintf(f, "bo2campath 1\ninterp %d\n", (int)interp);
        for (const auto& k : keys)
            std::fprintf(f, "cam %.3f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %d %.4f %.4f\n", k.timeMs, k.pose.pos.x, k.pose.pos.y,
                         k.pose.pos.z, k.pose.pitch, k.pose.yaw, k.pose.roll, k.pose.fov, k.ease, k.speed, k.tan);
        std::fclose(f);
        return true;
    }
    bool Load(const std::string& file) {
        FILE* f = std::fopen(file.c_str(), "rb");
        if (!f) return false;
        CamPath p;
        char line[512];
        bool header = false;
        while (std::fgets(line, sizeof line, f)) {
            if (!header) {
                            if (!std::strncmp(line, "bo2campath", 10)) header = true;
                else break;
                continue;
            }
            int ip;
            CamKey k;
            if (std::sscanf(line, "interp %d", &ip) == 1) p.interp = ip ? Interp::Smooth : Interp::Linear;
            else {
                int n = std::sscanf(line, "cam %lf %f %f %f %f %f %f %f %d %f %f", &k.timeMs, &k.pose.pos.x, &k.pose.pos.y, &k.pose.pos.z,
                                    &k.pose.pitch, &k.pose.yaw, &k.pose.roll, &k.pose.fov, &k.ease, &k.speed, &k.tan);
                if (n >= 8) p.keys.push_back(k);
            }
        }
        std::fclose(f);
        if (!header) return false;
        p.Sort();
        *this = p;
        return true;
    }
};

// ------------------------------------------------------------- freecam -----
struct FreeCamInput {
    float forward = 0, right = 0, up = 0;   // -1..1
    float rollDir = 0;                      // -1 / +1
    bool fast = false, slow = false;
};

struct FreeCamSettings {
    float moveSpeed = 260.0f;   // units / s  (move_speed)
    float fastMult = 3.5f;      // fast_mult
    float slowMult = 0.25f;     // slow_mult
    float rollSpeed = 60.0f;    // deg / s
    float smoothMove = 9.0f;    // 0 = off, higher = snappier
    float smoothLook = 20.0f;
};

// Exponential smoothing factor that is independent of the frame rate.
static inline float SmoothFactor(float rate, float dt) {
    return rate <= 0.0f ? 1.0f : 1.0f - std::exp(-rate * dt);
}

struct FreeCam {
    CamPose pose;      // what is actually shown
    CamPose target;    // where we are heading (position smoothing)
    Vec3 vel;

    void Reset(const CamPose& p) { pose = target = p; vel = Vec3(); }

    // angles come from the caller (game view angles + offset)
    void Step(float dt, const FreeCamInput& in, const FreeCamSettings& s, float pitch, float yaw) {
        float spd = s.moveSpeed * (in.fast ? s.fastMult : 1.0f) * (in.slow ? s.slowMult : 1.0f);
        Vec3 f, r, u;
        AngleVectors(pitch, yaw, target.roll, &f, &r, &u);
        // free movement follows the look direction; vertical is world up
        float mx = f.x * in.forward + r.x * in.right;
        float my = f.y * in.forward + r.y * in.right;
        float mz = f.z * in.forward + r.z * in.right + in.up;
        target.pos.x += mx * spd * dt;
        target.pos.y += my * spd * dt;
        target.pos.z += mz * spd * dt;
        target.roll = NormAngle180(target.roll + in.rollDir * s.rollSpeed * dt);
        float k = SmoothFactor(s.smoothMove, dt);
        pose.pos.x += (target.pos.x - pose.pos.x) * k;
        pose.pos.y += (target.pos.y - pose.pos.y) * k;
        pose.pos.z += (target.pos.z - pose.pos.z) * k;
        float kl = SmoothFactor(s.smoothLook, dt);
        pose.pitch = LerpAngle(pose.pitch, pitch, kl);
        pose.yaw = LerpAngle(pose.yaw, yaw, kl);
        pose.roll = LerpAngle(pose.roll, target.roll, kl);
        pose.fov = target.fov;
    }
};

}  // namespace mvm
