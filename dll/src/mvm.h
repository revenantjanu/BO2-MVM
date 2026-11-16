// mvm.h - shared state between the game-thread hook (core.cpp) and the overlay (overlay.cpp)
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <windows.h>
#include <string>
#include <mutex>
#include <vector>
#include "camera.h"
#include "settings.h"

namespace mvm {

struct State {
    std::mutex mx;                 // guards everything below except the atomics-by-convention flags
    Settings cfg;
    std::string dir;               // folder of the dll, with trailing backslash
    HWND hwnd = nullptr;

    // status shown in the menu
    bool lanOk = false;
    bool hooksOk = false;
    std::string status = "starting";

    // freecam
    bool freecam = false;
    bool frozen = false;
    bool menuOpen = false;
    FreeCam cam;
    FreeCamSettings camCfg;
    float angOffPitch = 0, angOffYaw = 0;   // freecam angles = game view angles + offset
    bool holdAngles = false;                // true while menu is open (keeps camera still)
    CamPose heldPose;
    bool wantEnter = false;                 // set by UI thread, consumed on game thread
    bool wantLeave = false;

    // timescale (what we last asked the game for)
    float timescale = 1.0f;
    float timescaleBeforeFreeze = 1.0f;

    // camera path / timeline
    CamPath path;
    bool playing = false;
    bool loop = false;
    double timeMs = 0;
    bool wantAddKey = false;
    bool wantSample = false;        // UI scrubbed the timeline: show the pose at timeMs once
    double keyTimeStep = 1000.0;

    // freecam recording (F9): samples the camera every ~33 ms, becomes the path when stopped
    bool recording = false;
    double recMs = 0, recLastKeyMs = -1000;
    std::vector<CamKey> recKeys;

    // lock-on: the camera keeps looking at a target while it moves (modes: 0 off, 1 fixed point, 2 selected zombie, 3 me)
    int lockMode = 0;
    int lockId = 0;                      // zombie id for mode 2
    Vec3 lockPoint;
    float lockSmooth = 10.0f, lockOffYaw = 0, lockOffPitch = 0;
    float lockYaw = 0, lockPitch = 0; bool lockInit = false;
    float lockDist = 0;                  // distance to the target this frame (for dolly zoom / focus)
    bool lockValid = false;
    bool dolly = false; float dollyK = 0;   // dolly zoom: fov follows distance so the subject keeps its size
    // path editing history + beat markers + scrubber
    std::vector<CamPath> undo, redo;
    std::vector<double> beats;
    float rampMul = 1.0f, rampBase = 1.0f;
    bool wasRamping = false;

    // orbit shot: the camera circles a point in front of where it was when you switched it on
    bool orbit = false;
    Vec3 orbitCenter;
    float orbitRadius = 400, orbitAng = 0, orbitSpeed = 20, orbitHeight = 0;

    // fov we want while in freecam
    float fov = 65.0f;

    // capture request flags
    bool wantCaptureStart = false;
    bool wantCaptureStop = false;
    bool capturing = false;
};

extern State g;
extern bool g_liveFxOk;    // the live shader pass ran this frame (so the video must not bake the same effects again)
void LiveFxPresent(void* swapChain);
extern bool g_liveGrade;   // colour preview done by the game (film tweaks) instead of baked into the video

// Plutonium runs the game inside plutonium-bootstrapper-win32.exe, so the main module may not be the game image.
// The game image always sits at its preferred base 0x400000; prefer that when it is a valid PE.
inline HMODULE GameModule() {
    __try {
        if (!IsBadReadPtr((void*)0x400000, 0x1000) && *(uint16_t*)0x400000 == 0x5A4D) return (HMODULE)0x400000;
    } __except (1) {}
    return GetModuleHandleA(nullptr);
}

inline void Log(const char* fmt, ...) {
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    FILE* f = nullptr; fopen_s(&f, (g.dir + "bo2mvm_log.txt").c_str(), "a");
    if (f) { fprintf(f, "%s\n", b); fclose(f); }
}

// core.cpp (game side)
bool CoreInit(HMODULE self);           // resolves addresses, installs the CalcView hook
void CoreShutdown();
void QueueCmd(const char* fmt, ...);   // thread safe, executed on the game thread
void CoreProbe();
void RequestEnterFreecam();
void RequestLeaveFreecam();
void SetTimescale(float ts);
void SetTimescaleFast(float ts);
void CaptureStill();                   // saves the next frame as a picture (BMP) in the captures folder
void StartOrbit(float radius);
bool InGame();                         // a match / demo is on screen
void PathPush();                       // remember the path before an edit (undo)
void PathUndo();
void PathRedo();
void ApplyRig(int rig);                // builds a path from the camera's current pose
void LockOn(int mode, int id);         // start / stop lock-on (mode 0 = off)
void StartDollyZoom();
bool TrackGet(Vec3* p, int slot = 0, float* extra = nullptr);   // where the tracked zombie / player is (slot 0 lock-on, 1 focus); false if unknown
void TrackSet(int id, int slot = 0);   // id 0 = me, -2 = middle of all zombies, -1 = stop
int TrackState();                      // 0 not looked up yet, 1 searching, 2 ready, 3 failed
void ZmSceneSave(const std::string& name);
bool ZmSceneLoad(const std::string& name);
std::vector<std::string> ZmSceneList();
int ZmSelectedId();
void ToggleFreeze();
void AddKeyFromCurrent();
bool KeyPressedEdge(int vk);           // overlay key-edge helper

// zombies.cpp (spawner + action editor, talks to scripts/zm/mvm_zm.gsc through the mvm_cmd dvar)
void ZmSend(const std::string& cmd);
void ZombiesUpdate(void* swapChain);
void DrawZombiesTab();
bool ZmHasSeq();
bool ZmSceneDirty();
void ZmSeqStart();
void ZmSeqStop();
void ZmSceneReset();
int ZmSeqTimes(float* t, const char** label, int maxN);
void ZmQuickSpawn();     // hotkey N: one zombie at the crosshair
void ZmQuickMove();      // hotkey M: selected zombie to the crosshair
void ZmQuickFreeze();    // hotkey U: freeze / unfreeze all

// overlay.cpp
void UiCardBegin(const char* title, const char* id);
void UiCardEnd();
void UiCols2Begin(const char* id);
void UiColNext();
void UiCols2End();
bool OverlayInit();
void OverlayShutdown();

// capture.cpp
void CaptureFramePresent(void* swapChain);   // called from the Present hook before the UI is drawn
void CaptureStart();
void CaptureStop();
bool CaptureActive();
unsigned CaptureFrames();
unsigned CaptureDupes();
std::string CaptureLastFile();
bool FfmpegFound();
int FfmpegInstallState();
void FfmpegInstall();
bool GrabThumb(void* swapChain, int out, float cropSide, float cropTop, std::vector<uint8_t>* rgba);

}  // namespace mvm
