// core.cpp - game side: address resolution, CalcViewValues hook, freecam / camera path driver.
//
// Addresses below (t6mp.exe values; t6zm.exe is found by signature scan: CalcView 0x7D4BF0, Cbuf 0x4C7120, Axis 0x429360) were taken from t6mp.exe (Plutonium BO2 multiplayer, 32 bit, image base 0x400000):
//
//   CG_CalcViewValues  0x7D5360  cdecl int(int localClientNum). Called once per frame on the game thread,
//                                it fills cg_t.refdef (vieworg + viewaxis). We call the original, then
//                                overwrite the view when the freecam / camera path is active.
//   cg_t* pointer      0x113F18C (data) - read out of the first instructions of CalcViewValues.
//   refdef.vieworg     cg + 0x4D8C4  (3 floats)   - confirmed: CalcViewValues passes &cg[0x4D8C4] as the origin
//   refdef.viewaxis    cg + 0x4D8D4  (3x3 floats) - confirmed: CG_OffsetThirdPersonView finishes with
//                                AnglesToAxis(angles, cg + 0x4D8D4)
//   AnglesToAxis       0x4D01F0  cdecl void(const float angles[3], float axis[9])
//   Cbuf_AddText       0x5BDF70  cdecl void(int localClientNum, const char* text)  (the same call the engine
//                                itself uses for "disconnect\n", "vid_restart\n", "snd_restart\n", ...)
//
// Every address is verified against a byte signature at startup. If Plutonium changes the binary the
// signature is searched in .text; if that fails too the tool disables itself instead of crashing the game.
#include "mvm.h"
#include <tlhelp32.h>
#include <winternl.h>
#include <cctype>
#include <cstdint>
#include <cstdarg>
#include <vector>
#include <cstdio>

namespace mvm {

State g;

namespace {

constexpr uintptr_t kImageBase = 0x400000;

struct Sig { const char* name; uintptr_t va; const char* pattern; };
// "??" = wildcard byte
// per-frame view setup (writes refdef vieworg/viewaxis, calls AnglesToAxis). Unique in t6zm live (0x7D7370), t6zm disk (0x7D60D0), t6mp (0x7D6830)
const Sig kCalcView = {"CG_CalcViewValues", 0x7D7370, "0F 57 C0 83 EC 08 53 8B 5C 24 10 56 8B 35 ?? ?? ?? ?? 8B 86 8C 80 04 00"};
constexpr size_t kStolen = 6;   // xorps xmm0,xmm0 ; sub esp,8
const Sig kCbuf     = {"Cbuf_AddText",      0x5BDF70, "6A 33 E8 ?? ?? ?? ?? 8B 4C 24 0C 83 C4 04 8B D1 85 C9 74 1F"};
const Sig kAxis     = {"AnglesToAxis",      0x4D01F0, "83 EC 20 F3 0F 10 0D ?? ?? ?? ?? 56 8B 74 24 28"};

constexpr uint32_t kOffVieworg = 0x4D8C4;
constexpr uint32_t kOffViewaxis = 0x4D8D4;

typedef int (__cdecl* CalcViewFn)(int);
typedef void (__cdecl* CbufFn)(int, const char*);
typedef void (__cdecl* AxisFn)(const float*, float*);

CalcViewFn g_origCalc = nullptr;
CbufFn g_cbuf = nullptr;
AxisFn g_axis = nullptr;
uint8_t* g_calcAddr = nullptr;
uint8_t g_savedBytes[8];
uint8_t* g_trampoline = nullptr;
uint8_t** g_cgPtr = nullptr;

std::mutex g_cmdMx;
std::vector<std::string> g_cmds;

LARGE_INTEGER g_freq, g_last;
float g_lastSentFov = -1;
bool g_prevKey[256] = {};

// ---------------------------------------------------------------- scanning -
bool MatchAt(const uint8_t* p, const char* pat) {
    while (*pat) {
        if (*pat == ' ') { ++pat; continue; }
        if (pat[0] == '?') { pat += 2; ++p; continue; }
        char b[3] = {pat[0], pat[1], 0};
        if (*p != (uint8_t)std::strtoul(b, nullptr, 16)) return false;
        pat += 2; ++p;
    }
    return true;
}

uint8_t* ScanText(const char* pat) {
    HMODULE m = GameModule();
    auto* dos = (IMAGE_DOS_HEADER*)m;
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)m + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".text", 5) != 0) continue;
        uint8_t* b = (uint8_t*)m + sec->VirtualAddress;
        DWORD n = sec->Misc.VirtualSize;
        for (DWORD o = 0; o + 64 < n; ++o)
            if (MatchAt(b + o, pat)) return b + o;
    }
    return nullptr;
}

uint8_t* Resolve(const Sig& s) {
    uintptr_t base = (uintptr_t)GameModule();
    uint8_t* expect = (uint8_t*)(base + (s.va - kImageBase));
    if (!IsBadReadPtr(expect, 32) && MatchAt(expect, s.pattern)) return expect;
    uint8_t* found = ScanText(s.pattern);
    if (!found) g.status = std::string("signature not found: ") + s.name;
    return found;
}

// ------------------------------------------------------------------ detour -
bool InstallDetour(uint8_t* target, void* hook, size_t stolen, uint8_t** trampolineOut) {
    uint8_t* tr = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    std::memcpy(tr, target, stolen);
    tr[stolen] = 0xE9;
    *(int32_t*)(tr + stolen + 1) = (int32_t)((target + stolen) - (tr + stolen + 5));
    DWORD old;
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(g_savedBytes, target, stolen);
    target[0] = 0xE9;
    *(int32_t*)(target + 1) = (int32_t)((uint8_t*)hook - (target + 5));
    for (size_t i = 5; i < stolen; ++i) target[i] = 0x90;
    VirtualProtect(target, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    *trampolineOut = tr;
    return true;
}

void RemoveDetour(uint8_t* target, size_t stolen) {
    DWORD old;
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old)) return;
    std::memcpy(target, g_savedBytes, stolen);
    VirtualProtect(target, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
}

// ---------------------------------------------------------------- helpers --
bool Down(int vk) {
    return vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000);
}

void FlushCommands() {
    std::vector<std::string> local;
    { std::lock_guard<std::mutex> l(g_cmdMx); local.swap(g_cmds); }
    for (auto& c : local) {
        Log("cmd: %s", c.c_str());
        std::string t = c + "\n";
        g_cbuf(0, t.c_str());
    }
}

void SendFov(float fov) {
    if (std::fabs(fov - g_lastSentFov) < 0.05f) return;
    g_lastSentFov = fov;
    QueueCmd("cg_fov %.2f", fov);
}

void EnterFreecam(uint8_t* cg) {
    Log("EnterFreecam cg=%p", (void*)cg);
    float* org = (float*)(cg + kOffVieworg);
    float* ax = (float*)(cg + kOffViewaxis);
    CamPose p;
    p.pos = {org[0], org[1], org[2]};
    Vec3 f = {ax[0], ax[1], ax[2]};
    ForwardToAngles(f, &p.pitch, &p.yaw);
    p.fov = g.cfg.default_fov;
    g.cam.Reset(p);
    g.angOffPitch = g.angOffYaw = 0;
    g.fov = p.fov;
    g_lastSentFov = -1;
    g.freecam = true;
    if (g.cfg.hide_hud_in_free) { QueueCmd("cg_draw2D 0"); QueueCmd("cg_drawGun 0"); }
    QueueCmd("cg_thirdPerson 0");
}

void LeaveFreecam() {
    g.freecam = false;
    g.orbit = false;
    g.lockMode = 0; g.dolly = false; TrackSet(-1);
    g.playing = false;
    if (g.cfg.hide_hud_in_free) { QueueCmd("cg_draw2D 1"); QueueCmd("cg_drawGun 1"); }
    QueueCmd("cg_fov %d", g.cfg.restore_fov);
}

void ApplyView(uint8_t* cg, float dt) {
    float* org = (float*)(cg + kOffVieworg);
    float* ax = (float*)(cg + kOffViewaxis);

    // orientation the *game* computed for the player (mouse look included)
    float gp, gy;
    ForwardToAngles({ax[0], ax[1], ax[2]}, &gp, &gy);

    bool menu = g.menuOpen;
    bool fg = g.hwnd && GetForegroundWindow() == g.hwnd;
    CamPose out = g.cam.pose;

    if (g.wantAddKey) {
        g.wantAddKey = false;
        CamKey k;
        k.pose = g.cam.pose;
        k.pose.fov = g.fov;
        k.timeMs = g.path.keys.empty() ? 0.0 : g.path.keys.back().timeMs + g.keyTimeStep;
        PathPush();
        g.path.keys.push_back(k);
    }

    bool pathDriving = false;
    if (g.playing && !g.path.keys.empty()) {
        double step = dt * 1000.0 * (g.cfg.path_follows_timescale ? g.timescale : 1.0);
        g.timeMs += step;
        double dur = g.path.Duration();
        if (g.timeMs > dur) {
            if (g.loop && dur > 0) g.timeMs = std::fmod(g.timeMs, dur);
            else { g.timeMs = dur; g.playing = false; }
        }
        pathDriving = true;
    } else if (g.wantSample && !g.path.keys.empty()) {
        pathDriving = true;
    }
    g.wantSample = false;

    {   // speed ramp: slow-motion (or fast) that eases along the path
        double durR = g.path.Duration();
        bool ramp = (g.cfg.ramp_on || g.path.HasSpeed()) && g.playing && durR > 0 && !g.frozen;
        if (ramp) {
            if (!g.wasRamping) g.rampBase = g.timescale;
            double u = std::max(0.0, std::min(1.0, g.timeMs / durR));
            float w = (float)(u * u * (3 - 2 * u));
            float glob = g.cfg.ramp_on ? Lerp(g.cfg.ramp_start, g.cfg.ramp_end, w) : 1.0f;
            g.rampMul = std::max(0.02f, glob * g.path.SampleSpeed(g.timeMs));
            if (!g.capturing) {
                float want = g.rampBase * g.rampMul;
                if (std::fabs(want / std::max(0.0001f, g.timescale) - 1.0f) > 0.02f) SetTimescaleFast(want);
            }
            g.wasRamping = true;
        } else {
            if (g.wasRamping && !g.capturing && !g.frozen) SetTimescaleFast(g.rampBase);
            g.rampMul = 1.0f;
            g.wasRamping = false;
        }
    }

    if (pathDriving) {
        CamPose p;
        double dur0 = g.path.Duration(), tw = g.timeMs;
        if (dur0 > 0 && g.cfg.path_ease) {          // ease the whole path in / out
            double u = std::max(0.0, std::min(1.0, tw / dur0));
            double w = g.cfg.path_ease == 1 ? u * u * (3 - 2 * u) : g.cfg.path_ease == 2 ? u * u : 1 - (1 - u) * (1 - u);
            tw = w * dur0;
        }
        if (g.path.Sample(tw, &p)) {
            g.cam.pose = g.cam.target = p;
            g.fov = p.fov;
            g.angOffPitch = p.pitch - gp;
            g.angOffYaw = p.yaw - gy;
        }
        out = g.cam.pose;
    } else {
        float pitch, yaw;
        if (menu) {                      // keep the camera still while the menu is open
            pitch = g.cam.pose.pitch;
            yaw = g.cam.pose.yaw;
            g.angOffPitch = pitch - gp;
            g.angOffYaw = yaw - gy;
        } else {
            pitch = std::max(-89.0f, std::min(89.0f, gp + g.angOffPitch));
            yaw = NormAngle180(gy + g.angOffYaw);
        }
        FreeCamInput in;
        if (!menu && fg) {
            in.forward = (Down('W') ? 1.f : 0.f) - (Down('S') ? 1.f : 0.f);
            in.right = (Down('D') ? 1.f : 0.f) - (Down('A') ? 1.f : 0.f);
            in.up = ((Down(VK_SPACE) || Down('E')) ? 1.f : 0.f) - ((Down('Q') || Down('C')) ? 1.f : 0.f);
            if (Down(VK_OEM_PLUS) || Down(VK_ADD))   g.fov = std::min(160.0f, g.fov + 40.0f * dt);
            if (Down(VK_OEM_MINUS) || Down(VK_SUBTRACT)) g.fov = std::max(10.0f, g.fov - 40.0f * dt);
            in.rollDir = (Down(g.cfg.roll_right_key) ? 1.f : 0.f) - (Down(g.cfg.roll_left_key) ? 1.f : 0.f);
            in.fast = Down(VK_LSHIFT);
            in.slow = Down(VK_LCONTROL) || Down(VK_LMENU);   // 6x: Ctrl = slower
        }
        FreeCamSettings cs;
        cs.moveSpeed = g.cfg.move_speed; cs.fastMult = g.cfg.fast_mult; cs.slowMult = g.cfg.slow_mult;
        cs.rollSpeed = g.cfg.roll_speed; cs.smoothMove = g.cfg.smooth_move; cs.smoothLook = g.cfg.smooth_look;
        g.cam.target.fov = g.fov;
        g.cam.Step(dt, in, cs, pitch, yaw);
        out = g.cam.pose;
    }

    if (g.orbit && !pathDriving) {           // orbit shot: circle the centre, always looking at it
        g.orbitAng += g.orbitSpeed * dt;
        float a = Deg2Rad(g.orbitAng);
        out.pos.x = g.orbitCenter.x + std::cos(a) * g.orbitRadius;
        out.pos.y = g.orbitCenter.y + std::sin(a) * g.orbitRadius;
        out.pos.z = g.orbitCenter.z + g.orbitHeight;
        float dx = g.orbitCenter.x - out.pos.x, dy = g.orbitCenter.y - out.pos.y, dz = g.orbitCenter.z - out.pos.z;
        out.yaw = Rad2Deg(std::atan2(dy, dx));
        out.pitch = Rad2Deg(std::atan2(-dz, std::sqrt(dx * dx + dy * dy)));
        g.cam.pose = g.cam.target = out;      // leaving orbit keeps this position
        g.angOffPitch = out.pitch - gp;
        g.angOffYaw = NormAngle180(out.yaw - gy);
    }

    if (g.recording) {                       // F9: sample the camera at ~30 Hz
        g.recMs += dt * 1000.0;
        if (g.recMs - g.recLastKeyMs >= 33.0) {
            CamKey k; k.pose = out; k.pose.fov = g.fov; k.timeMs = g.recMs;
            g.recKeys.push_back(k);
            g.recLastKeyMs = g.recMs;
        }
    }

    g.lockValid = false;
    if (g.lockMode) {                        // lock-on: keep the target in frame whatever the camera does
        Vec3 tp; bool ok = false;
        if (g.lockMode == 1) { tp = g.lockPoint; ok = true; }
        else ok = TrackGet(&tp);
        if (ok) {
            float dx = tp.x - out.pos.x, dy = tp.y - out.pos.y, dz = tp.z - out.pos.z;
            float hh = std::sqrt(dx * dx + dy * dy);
            float ty = Rad2Deg(std::atan2(dy, dx)), tpc = Rad2Deg(std::atan2(-dz, hh));
            if (!g.lockInit) { g.lockYaw = ty; g.lockPitch = tpc; g.lockInit = true; }
            else { float k = SmoothFactor(g.lockSmooth, dt); g.lockYaw += NormAngle180(ty - g.lockYaw) * k; g.lockPitch += (tpc - g.lockPitch) * k; }
            out.yaw = g.lockYaw + g.lockOffYaw;
            out.pitch = g.lockPitch + g.lockOffPitch;
            g.lockDist = std::sqrt(dx * dx + dy * dy + dz * dz);
            g.lockValid = true;
            if (g.dolly && g.lockDist > 1.0f) out.fov = std::max(8.0f, std::min(150.0f, 2.0f * Rad2Deg(std::atan(g.dollyK / g.lockDist))));
        }
    }

    if (g.cfg.shake > 0.001f) {              // handheld shake: only on what the game sees, never in the path
        static double st = 0; st += dt * g.cfg.shake_speed;
        auto n = [&](double f, double ph) { return (float)(std::sin(st * f + ph) * 0.6 + std::sin(st * f * 2.3 + ph * 1.7) * 0.3 + std::sin(st * f * 4.1 + ph * 0.6) * 0.1); };
        float k = g.cfg.shake;
        out.pos.x += n(2.1, 0.0) * 1.2f * k; out.pos.y += n(1.7, 2.0) * 1.2f * k; out.pos.z += n(2.6, 4.0) * 0.9f * k;
        out.pitch += n(1.9, 1.0) * 0.55f * k; out.yaw += n(1.4, 3.0) * 0.55f * k; out.roll += n(1.1, 5.0) * 0.45f * k;
    }
    org[0] = out.pos.x; org[1] = out.pos.y; org[2] = out.pos.z;
    float angles[3] = {out.pitch, out.yaw, out.roll};
    g_axis(angles, ax);
    SendFov(out.fov);
}

void Tick() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = (float)((double)(now.QuadPart - g_last.QuadPart) / (double)g_freq.QuadPart);
    g_last = now;
    if (dt < 0) dt = 0;
    if (dt > 0.1f) dt = 0.1f;

    {
        std::lock_guard<std::mutex> l(g.mx);
        uint8_t* cg = g_cgPtr ? *g_cgPtr : nullptr;
        if (cg) {
            if (g.wantEnter) { g.wantEnter = false; if (!g.freecam) EnterFreecam(cg); }
            if (g.wantLeave) { g.wantLeave = false; if (g.freecam) LeaveFreecam(); }
            if (g.freecam) ApplyView(cg, dt);
        } else {
            if (g.wantEnter) Log("freecam requested but cg_t pointer is NULL (not in a map?)");
            g.wantEnter = g.wantLeave = false;
        }
    }
    FlushCommands();
}

static volatile unsigned g_calcCalls = 0;
int __cdecl hkCalcView(int localClient) {
    static unsigned calls = 0;
    g_calcCalls = calls + 1;
    int r = g_origCalc(localClient);
    ++calls;
    if (calls <= 5 || calls % 1800 == 0) Log("hkView call #%u localClient=%d cg=%p", calls, localClient, g_cgPtr ? (void*)*g_cgPtr : nullptr);
    Tick();
    return r;
}

}  // namespace

// ------------------------------------------------------------ public API ---
static void DumpLiveText() {
    // writes the live .text of the game image (0x401000, 7.6 MB) so the real code can be analysed offline
    const uintptr_t start = 0x401000, size = 7663616;
    FILE* f = nullptr;
    fopen_s(&f, (g.dir + "t6zm_live_text.bin").c_str(), "wb");
    if (!f) { Log("dump: cannot open file"); return; }
    uintptr_t p = start; size_t wrote = 0;
    while (p < start + size) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)p, &mbi, sizeof mbi)) break;
        uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (end > start + size) end = start + size;
        size_t len = end - p;
        bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        if (ok) fwrite((void*)p, 1, len, f);
        else { std::vector<char> z(len, 0); fwrite(z.data(), 1, len, f); }
        wrote += len; p = end;
    }
    fclose(f);
    Log("dump: wrote %zu bytes of live .text starting at 0x401000", wrote);
}

void CoreProbe() {
    static unsigned n = 0;
    ++n;
    if (n == 3) DumpLiveText();
    uint8_t* cg = g_cgPtr && !IsBadReadPtr(g_cgPtr, 4) ? *g_cgPtr : nullptr;
    float org[3] = {0, 0, 0}; int st = -1;
    if (cg && !IsBadReadPtr(cg + kOffVieworg, 12)) std::memcpy(org, cg + kOffVieworg, 12);
    if (cg && !IsBadReadPtr(cg + 0x480A0, 4)) st = *(int*)(cg + 0x480A0);
    char hex[40] = {};
    if (g_calcAddr && !IsBadReadPtr(g_calcAddr, 8)) std::snprintf(hex, sizeof hex, "%02X %02X %02X %02X %02X", g_calcAddr[0], g_calcAddr[1], g_calcAddr[2], g_calcAddr[3], g_calcAddr[4]);
    Log("probe #%u calcCalls=%u cgPtrAddr=%p cg=%p cg+480A0=%d vieworg=%.1f %.1f %.1f calcBytes=%s", n, g_calcCalls, (void*)g_cgPtr, (void*)cg, st, org[0], org[1], org[2], hex);
}

void QueueCmd(const char* fmt, ...) {
    char buf[512];
    va_list va;
    va_start(va, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, va);
    va_end(va);
    std::lock_guard<std::mutex> l(g_cmdMx);
    g_cmds.emplace_back(buf);
}

void RequestEnterFreecam() { std::lock_guard<std::mutex> l(g.mx); g.wantEnter = true; g.wantLeave = false; }
void RequestLeaveFreecam() { std::lock_guard<std::mutex> l(g.mx); g.wantLeave = true; g.wantEnter = false; }
void AddKeyFromCurrent() { std::lock_guard<std::mutex> l(g.mx); if (g.freecam) g.wantAddKey = true; }

static void SendTimescale(float v) {
    // BO2: "timescale" is the cheat-protected dvar the game honours; com_timescale is sent too for builds that use it
    QueueCmd("sv_cheats 1");
    QueueCmd("timescale %.5f", v);
    QueueCmd("com_timescale %.5f", v);
}

void SetTimescale(float ts) {
    g.timescale = ts;
    SendTimescale(ts);
}
void SetTimescaleFast(float ts) {      // used every few frames by the fixed-step render, so no sv_cheats spam
    g.timescale = ts;
    QueueCmd("timescale %.5f", ts);
    QueueCmd("com_timescale %.5f", ts);
}
void StartOrbit(float radius) {        // center = the point `radius` units in front of the camera
    std::lock_guard<std::mutex> l(g.mx);
    Vec3 f, r, u;
    AngleVectors(g.cam.pose.pitch, g.cam.pose.yaw, 0, &f, &r, &u);
    g.orbitRadius = radius;
    g.orbitCenter.x = g.cam.pose.pos.x + f.x * radius;
    g.orbitCenter.y = g.cam.pose.pos.y + f.y * radius;
    g.orbitCenter.z = g.cam.pose.pos.z + f.z * radius;
    g.orbitHeight = g.cam.pose.pos.z - g.orbitCenter.z;
    g.orbitAng = Rad2Deg(std::atan2(g.cam.pose.pos.y - g.orbitCenter.y, g.cam.pose.pos.x - g.orbitCenter.x));
    g.orbit = true;
}
bool InGame() { uint8_t* cg = g_cgPtr && !IsBadReadPtr(g_cgPtr, 4) ? *g_cgPtr : nullptr; return cg != nullptr; }
void PathPush() {
    g.undo.push_back(g.path);
    if (g.undo.size() > 60) g.undo.erase(g.undo.begin());
    g.redo.clear();
}
void PathUndo() {
    if (g.undo.empty()) return;
    g.redo.push_back(g.path);
    g.path = g.undo.back(); g.undo.pop_back();
}
void PathRedo() {
    if (g.redo.empty()) return;
    g.undo.push_back(g.path);
    g.path = g.redo.back(); g.redo.pop_back();
}

void LockOn(int mode, int id) {
    g.lockMode = mode; g.lockId = id; g.lockInit = false; g.lockValid = false; g.dolly = false;
    if (mode == 1) {                        // fixed point 500 units in front of the camera
        Vec3 f;
        AngleVectors(g.cam.pose.pitch, g.cam.pose.yaw, 0, &f, nullptr, nullptr);
        g.lockPoint.x = g.cam.pose.pos.x + f.x * 500; g.lockPoint.y = g.cam.pose.pos.y + f.y * 500; g.lockPoint.z = g.cam.pose.pos.z + f.z * 500;
    }
    if (mode == 2) TrackSet(id);
    else if (mode == 3) TrackSet(0);
    else TrackSet(-1);
}
void StartDollyZoom() {
    if (!g.lockValid || g.lockDist < 1.0f) return;
    g.dollyK = std::tan(Deg2Rad(g.fov * 0.5f)) * g.lockDist;
    g.dolly = true;
}

// camera rigs: ready-made moves built from wherever the camera is now
void ApplyRig(int rig) {
    PathPush();
    CamPose p0 = g.cam.pose; p0.fov = g.fov; p0.roll = g.cam.pose.roll;
    Vec3 f, r, u;
    AngleVectors(p0.pitch, p0.yaw, 0, &f, &r, &u);
    g.path.keys.clear();
    auto mk = [&](double t, const CamPose& p, int ease, float tn) { CamKey k; k.timeMs = t; k.pose = p; k.ease = ease; k.tan = tn; g.path.keys.push_back(k); };
    auto move = [&](CamPose p, const Vec3& d, float s) { p.pos.x += d.x * s; p.pos.y += d.y * s; p.pos.z += d.z * s; return p; };
    CamPose p1 = p0;
    switch (rig) {
        case 0: p1 = move(p0, f, 380); p1.fov = std::max(20.0f, p0.fov - 8); mk(0, p0, 1, 1); mk(3500, p1, 0, 1); break;      // push in
        case 1: p1 = move(p0, f, -380); p1.fov = std::min(120.0f, p0.fov + 8); mk(0, p0, 1, 1); mk(3500, p1, 0, 1); break;   // pull out
        case 2: p1 = move(p0, u, 320); p1.pitch = p0.pitch + 20; mk(0, p0, 1, 1); mk(4000, p1, 0, 1); break;                 // crane up
        case 3: p1 = move(p0, r, 420); mk(0, p0, 1, 1); mk(3500, p1, 0, 1); break;                                          // slide right
        case 4: p1 = move(p0, r, -420); mk(0, p0, 1, 1); mk(3500, p1, 0, 1); break;                                         // slide left
        case 5: {                                                                                                           // whip pan
            CamPose w = p0; w.yaw = p0.yaw + 140;
            mk(0, p0, 4, 1); mk(900, p0, 1, 1); mk(1250, w, 4, 1); mk(3000, w, 0, 1);
            break;
        }
        case 6: p1 = move(p0, f, 300); p1.roll = p0.roll + 14; p1.fov = std::max(20.0f, p0.fov - 10); mk(0, p0, 1, 1); mk(3500, p1, 0, 1); break;   // dutch push
        case 7: {                                                                                                           // arc around the point in front
            Vec3 c = move(p0, f, 420).pos;
            float a0 = Rad2Deg(std::atan2(p0.pos.y - c.y, p0.pos.x - c.x));
            for (int i = 0; i < 5; ++i) {
                float a = Deg2Rad(a0 - 40 + 20 * i);
                CamPose p = p0;
                p.pos.x = c.x + std::cos(a) * 420; p.pos.y = c.y + std::sin(a) * 420; p.pos.z = p0.pos.z;
                float dx = c.x - p.pos.x, dy = c.y - p.pos.y, dz = c.z - p.pos.z;
                p.yaw = Rad2Deg(std::atan2(dy, dx)); p.pitch = Rad2Deg(std::atan2(-dz, std::sqrt(dx * dx + dy * dy)));
                mk(i * 1000.0, p, 0, 1);
            }
            break;
        }
    }
    g.path.interp = Interp::Smooth;
    g.timeMs = 0;
}

void ToggleFreeze() {
    if (!g.frozen) { g.timescaleBeforeFreeze = g.timescale; g.frozen = true; SendTimescale(g.cfg.frozen_timescale); g.timescale = g.cfg.frozen_timescale; }
    else { g.frozen = false; SetTimescale(g.timescaleBeforeFreeze); }
}

bool KeyPressedEdge(int vk) {
    if (vk <= 0 || vk >= 256) return false;
    bool d = Down(vk);
    bool e = d && !g_prevKey[vk];
    g_prevKey[vk] = d;
    return e;
}

// ---- LAN detection: Plutonium's bootstrapper keeps "-lan" on its own command line; the game process often does not have it.
bool ReadProcCmdLine(DWORD pid, std::string& out) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return false;
    typedef LONG (WINAPI* NtQIP)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    NtQIP q = (NtQIP)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    bool ok = false;
    PROCESS_BASIC_INFORMATION pbi = {};
    ULONG rl = 0;
    if (q && q(h, 0, &pbi, sizeof pbi, &rl) >= 0 && pbi.PebBaseAddress) {
        uint8_t* peb = (uint8_t*)pbi.PebBaseAddress;
        uint32_t params = 0;
        if (ReadProcessMemory(h, peb + 0x10, &params, 4, nullptr) && params) {
            USHORT len = 0; uint32_t buf = 0;
            ReadProcessMemory(h, (void*)(uintptr_t)(params + 0x40), &len, 2, nullptr);
            ReadProcessMemory(h, (void*)(uintptr_t)(params + 0x44), &buf, 4, nullptr);
            if (len && buf && len < 8192) {
                std::vector<wchar_t> w(len / 2 + 1, 0);
                if (ReadProcessMemory(h, (void*)(uintptr_t)buf, w.data(), len, nullptr)) {
                    char tmp[4096] = {};
                    WideCharToMultiByte(CP_ACP, 0, w.data(), -1, tmp, sizeof tmp - 1, nullptr, nullptr);
                    out = tmp; ok = true;
                }
            }
        }
    }
    CloseHandle(h);
    return ok;
}

bool DetectLan() {
    const char* own = GetCommandLineA();
    Log("game cmdline: %s", own ? own : "(null)");
    if (own && std::strstr(own, "-lan")) return true;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe = {sizeof pe};
    DWORD me = GetCurrentProcessId();
    bool lan = false;
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
        bool parent = false;
        std::string name = pe.szExeFile;
        for (auto& c : name) c = (char)tolower((unsigned char)c);
        if (pe.th32ProcessID != me && name.find("lan launcher") != std::string::npos) { Log("LAN launcher running: %s", pe.szExeFile); lan = true; }
        parent = (pe.th32ProcessID != me) && (name.find("plutonium") != std::string::npos);
        if (!parent) continue;
        std::string cl;
        if (ReadProcCmdLine(pe.th32ProcessID, cl)) {
            Log("%s (pid %lu) cmdline: %s", pe.szExeFile, pe.th32ProcessID, cl.c_str());
            if (cl.find("-lan") != std::string::npos) lan = true;
        } else Log("%s (pid %lu): cmdline unreadable", pe.szExeFile, pe.th32ProcessID);
    }
    CloseHandle(snap);
    return lan;
}

bool CoreInit(HMODULE) {
    g.lanOk = DetectLan();
    if (!g.lanOk) { g.status = "disabled: no -lan found on the game or Plutonium launcher command line (see bo2mvm_log.txt)"; return false; }

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_last);

    uint8_t* calc = Resolve(kCalcView);
    uint8_t* cbuf = Resolve(kCbuf);
    uint8_t* axis = Resolve(kAxis);
    if (!calc || !cbuf || !axis) return false;

    // pointer to cg_t is the absolute address inside "mov esi, ds:[cg]" at calc+0x0B
    g_cgPtr = (uint8_t**)(*(uintptr_t*)(calc + 0x0E));
    // sanity: refdef.vieworg offset must still be referenced inside the function ("lea ebp,[esi+0x4D8C4]")
    bool offOk = false;
    for (int i = 0; i < 0x540 && !offOk; ++i)
        if (calc[i] == 0xC4 && calc[i + 1] == 0xD8 && calc[i + 2] == 0x04 && calc[i + 3] == 0x00) offOk = true;
    if (!offOk) { g.status = "refdef offset check failed (game updated?)"; return false; }

    g_cbuf = (CbufFn)cbuf;
    g_axis = (AxisFn)axis;
    g_calcAddr = calc;
    if (!InstallDetour(calc, (void*)hkCalcView, kStolen, &g_trampoline)) { g.status = "detour failed"; return false; }
    g_origCalc = (CalcViewFn)g_trampoline;
    g.hooksOk = true;
    g.status = "ready";
    return true;
}

void CoreShutdown() {
    if (g.hooksOk) {
        if (g.freecam) { g.freecam = false; QueueCmd("cg_draw2D 1"); QueueCmd("cg_drawGun 1"); }
        // restore game time and fov, then let one frame flush the commands
        SendTimescale(1.0f);
        Sleep(60);
        RemoveDetour(g_calcAddr, kStolen);
        g.hooksOk = false;
    }
}

}  // namespace mvm
