// overlay.cpp - DXGI Present hook (BO2 renders with Direct3D 11), ImGui menu, input handling.
#include "mvm.h"
#include <functional>
#include <cctype>
#include <algorithm>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <vector>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace mvm {
namespace {

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef BOOL(WINAPI* SetCursorPosFn)(int, int);
typedef BOOL(WINAPI* ClipCursorFn)(const RECT*);
typedef BOOL(WINAPI* GetCursorPosFn)(POINT*);

PresentFn g_origPresent = nullptr;
ResizeFn g_origResize = nullptr;
SetCursorPosFn g_origSetCursorPos = nullptr;
ClipCursorFn g_origClipCursor = nullptr;
GetCursorPosFn g_origGetCursorPos = nullptr;
POINT g_center = {0, 0};
bool g_haveCenter = false;
void** g_vtable = nullptr;
WNDPROC g_origWndProc = nullptr;

IDXGISwapChain* g_sc = nullptr;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_imguiReady = false;
bool g_shuttingDown = false;
bool g_showTimeline = true;
float g_recArm = 0;             // seconds until F9 recording starts
float g_prerollT = 0;           // pre-roll seconds left (render state 4)
int g_render = 0;               // path render: 0 idle, 1 waiting for the free camera, 2 waiting for the dolly to start, 3 capturing
double g_countdown = 0;          // seconds left before a take starts (menu is already closed)
bool g_plainTake = false;        // capture without a camera path
std::string g_toast; double g_toastT = 0;
float g_countdownLen = 3.0f;
bool g_hideHints = false;       // hide the on-screen text (top left) so nothing needs cropping out
bool g_syncActions = true;      // zombie action sequence starts together with the dolly
double g_armT = -1;             // seconds left before a re-armed take starts

static ULONGLONG g_armLast = 0;
// Start the dolly from the beginning. With "sync" the zombie scene is reset if needed and the action sequence starts at the same instant.
static void StartDolly() {
    if (g.path.keys.size() < 2) return;
    if (g.frozen) ToggleFreeze();                      // armed while the game was frozen: unfreeze, then everything goes at once
    if (g_syncActions && ZmHasSeq() && ZmSceneDirty()) {
        ZmSceneReset();
        g.playing = false; g.timeMs = 0; g_armT = 2.6; g_armLast = GetTickCount64();   // let the zombies respawn, then go
        return;
    }
    g.timeMs = 0; g.playing = true; g_armT = -1;
    if (g_syncActions) ZmSeqStart();
}
static void StopDolly() { g.playing = false; g_armT = -1; ZmSeqStop(); }
static void TickDolly() {
    if (g_armT < 0) return;
    ULONGLONG n = GetTickCount64();
    g_armT -= (double)(n - g_armLast) / 1000.0; g_armLast = n;
    if (g_armT <= 0) { g_armT = -1; g.timeMs = 0; g.playing = true; if (g_syncActions) ZmSeqStart(); }
}
char g_pathName[64] = "path1";
std::string g_pathMsg;

// ------------------------------------------------------------ IAT patch ---
bool PatchIAT(const char* dll, const char* func, void* hook, void** orig) {
    HMODULE m = GameModule();
    auto* dos = (IMAGE_DOS_HEADER*)m;
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)m + dos->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)((uint8_t*)m + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        if (_stricmp((const char*)((uint8_t*)m + imp->Name), dll) != 0) continue;
        auto* thunkName = (IMAGE_THUNK_DATA*)((uint8_t*)m + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* thunkAddr = (IMAGE_THUNK_DATA*)((uint8_t*)m + imp->FirstThunk);
        for (; thunkName->u1.AddressOfData; ++thunkName, ++thunkAddr) {
            if (IMAGE_SNAP_BY_ORDINAL(thunkName->u1.Ordinal)) continue;
            auto* ibn = (IMAGE_IMPORT_BY_NAME*)((uint8_t*)m + thunkName->u1.AddressOfData);
            if (std::strcmp(ibn->Name, func) != 0) continue;
            DWORD old;
            VirtualProtect(&thunkAddr->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            *orig = (void*)thunkAddr->u1.Function;
            thunkAddr->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&thunkAddr->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

// While the menu is open the game must not recentre / clip the cursor.
// The game reads mouse look as (GetCursorPos - centre) and then recentres with SetCursorPos. While the menu is open the real
// cursor must roam free, so the game is told the cursor is still exactly on its centre (zero delta, no spinning).
BOOL WINAPI hkSetCursorPos(int x, int y) {
    if (g.menuOpen) return TRUE;
    g_center.x = x; g_center.y = y; g_haveCenter = true;
    return g_origSetCursorPos(x, y);
}
BOOL WINAPI hkGetCursorPos(POINT* p) {
    if (g.menuOpen && g_haveCenter && p) { *p = g_center; return TRUE; }
    return g_origGetCursorPos(p);
}
BOOL WINAPI hkClipCursor(const RECT* r) { return g.menuOpen ? TRUE : g_origClipCursor(r); }

// ----------------------------------------------------------- window proc --
bool IsMoveKey(WPARAM vk) {
    return vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D' || vk == VK_SPACE || vk == VK_LCONTROL || vk == VK_CONTROL ||
           vk == VK_SHIFT || vk == VK_LSHIFT || vk == 'Q' || vk == 'E' || vk == 'C' || vk == 'F' || vk == 'G' || vk == 'K' || vk == 'J' || vk == 'L' || vk == 'N' || vk == 'M' || vk == 'U' || vk == 'R' || vk == 'H' || vk == 'V' || vk == 'B' ||
           (int)vk == g.cfg.roll_left_key || (int)vk == g.cfg.roll_right_key;
}

LRESULT CALLBACK hkWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g.menuOpen) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        switch (msg) {
            case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN:
            case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_CHAR: case WM_INPUT:
            case WM_KEYDOWN: case WM_SYSKEYDOWN:
                return 0;      // key *up* is passed on so the game never sees a stuck key
        }
    } else if (g.freecam) {
        // keep the player standing still while flying the camera (key-up still passes through)
        if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && IsMoveKey(wp)) return 0;
        if (msg == WM_MOUSEWHEEL) {      // 6x: wheel = FOV, Ctrl+wheel = speed, Alt+wheel = roll
            float d = (float)GET_WHEEL_DELTA_WPARAM(wp) / 120.0f;
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                float v = g.cfg.move_speed * std::pow(1.2f, d);
                g.cfg.move_speed = v < 10.f ? 10.f : (v > 6000.f ? 6000.f : v);
            } else if (GetKeyState(VK_MENU) & 0x8000) {
                g.cam.target.roll = NormAngle180(g.cam.target.roll + d * 3.0f);
            } else {
                g.fov = std::max(10.0f, std::min(160.0f, g.fov - d * 2.5f));
            }
            return 0;
        }
    }
    return CallWindowProcA(g_origWndProc, hwnd, msg, wp, lp);
}

// -------------------------------------------------------------- ImGui UI --
// Look and layout: near-black rounded window, icon rail on the left, two columns of cards
// (accent bar + title), toggle switches, pill selectors, value-above-slider rows, timeline docked at the bottom.
static ImVec4 g_accent = ImVec4(0.86f, 0.86f, 0.84f, 1.0f);
static int g_accentIdx = 0;
static const ImVec4 kAccents[] = {
    ImVec4(0.86f, 0.86f, 0.84f, 1), ImVec4(1.00f, 1.00f, 1.00f, 1), ImVec4(0.55f, 0.36f, 0.96f, 1),
    ImVec4(0.93f, 0.27f, 0.27f, 1), ImVec4(0.20f, 0.72f, 0.36f, 1), ImVec4(0.98f, 0.70f, 0.10f, 1)};

void StyleUi() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 8; s.ChildRounding = 6; s.FrameRounding = 4; s.GrabRounding = 4; s.PopupRounding = 5;
    s.WindowBorderSize = 1; s.ChildBorderSize = 1; s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(0, 0); s.ItemSpacing = ImVec2(8, 7); s.FramePadding = ImVec2(8, 5);
    s.ScrollbarSize = 8; s.ScrollbarRounding = 4;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.045f, 0.045f, 0.047f, 0.97f);
    c[ImGuiCol_ChildBg] = ImVec4(0.075f, 0.075f, 0.078f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.16f, 0.16f, 0.17f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.11f, 0.115f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.15f, 0.155f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.18f, 0.19f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.13f, 0.13f, 0.135f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.19f, 0.19f, 0.20f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.24f, 0.24f, 0.25f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.18f, 0.19f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.22f, 0.23f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.45f, 0.47f, 1.0f);
    c[ImGuiCol_SliderGrab] = g_accent;
    c[ImGuiCol_SliderGrabActive] = g_accent;
    c[ImGuiCol_CheckMark] = g_accent;
    c[ImGuiCol_Separator] = ImVec4(0.16f, 0.16f, 0.17f, 1.0f);
    ImGui::GetStyle().FontScaleMain = g.cfg.ui_scale;
}

std::string PathFile() { return g.dir + "paths\\" + g_pathName + ".bo2path"; }

void DrawHud() {
    // small indicator, drawn after capture so it never ends up in a video
    {
        ImDrawList* fd = ImGui::GetForegroundDrawList();
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        if (g_recArm > 0) {
            char cb[16]; std::snprintf(cb, sizeof cb, "%d", (int)std::ceil(g_recArm));
            ImFont* fnt = ImGui::GetFont(); float fs = 140.0f;
            ImVec2 ts = ImGui::CalcTextSize(cb); ts.x *= fs / ImGui::GetFontSize(); ts.y *= fs / ImGui::GetFontSize();
            fd->AddText(fnt, fs, ImVec2((ds.x - ts.x) * 0.5f, ds.y * 0.35f), IM_COL32(255, 90, 90, 230), cb);
            const char* m = "recording the camera - F9 cancels";
            ImVec2 ms = ImGui::CalcTextSize(m);
            fd->AddText(ImVec2((ds.x - ms.x) * 0.5f, ds.y * 0.35f + 150), IM_COL32(255, 255, 255, 220), m);
        }
        if (g.recording && !CaptureActive()) {
            char rb[64]; std::snprintf(rb, sizeof rb, "REC  %.1f s   (F9 to stop)", g.recMs / 1000.0);
            fd->AddCircleFilled(ImVec2(24, 26), 7.0f, IM_COL32(255, 50, 50, 255));
            fd->AddText(ImVec2(38, 17), IM_COL32(255, 255, 255, 235), rb);
        }
        if (g_countdown > 0) {
            char cb[16]; std::snprintf(cb, sizeof cb, "%d", (int)std::ceil(g_countdown));
            ImFont* fnt = ImGui::GetFont(); float fs = 140.0f;
            ImVec2 ts = ImGui::CalcTextSize(cb); ts.x *= fs / ImGui::GetFontSize(); ts.y *= fs / ImGui::GetFontSize();
            fd->AddText(fnt, fs, ImVec2((ds.x - ts.x) * 0.5f, ds.y * 0.35f), IM_COL32(255, 255, 255, 230), cb);
            const char* m = "get ready - F5 cancels";
            ImVec2 ms = ImGui::CalcTextSize(m);
            fd->AddText(ImVec2((ds.x - ms.x) * 0.5f, ds.y * 0.35f + fs + 8), IM_COL32(220, 220, 215, 230), m);
        }
        if (g.cfg.letterbox > 0 && !CaptureActive()) {                 // preview of the bars (the recorded video gets them baked in)
            static const float kr[] = {0, 2.39f, 2.0f, 1.85f};
            float vis = ds.x / kr[std::min(3, g.cfg.letterbox)];
            if (vis < ds.y) { float bar = (ds.y - vis) * 0.5f; fd->AddRectFilled(ImVec2(0, 0), ImVec2(ds.x, bar), IM_COL32(0, 0, 0, 255)); fd->AddRectFilled(ImVec2(0, ds.y - bar), ImVec2(ds.x, ds.y), IM_COL32(0, 0, 0, 255)); }
        }
        if (g.cfg.guides > 0 && !CaptureActive() && !g_hideHints) {
            ImU32 gc = IM_COL32(255, 255, 255, 90);
            if (g.cfg.guides == 2) { fd->AddLine(ImVec2(ds.x * 0.5f, 0), ImVec2(ds.x * 0.5f, ds.y), gc); fd->AddLine(ImVec2(0, ds.y * 0.5f), ImVec2(ds.x, ds.y * 0.5f), gc); }
            else {
                for (int i = 1; i <= 2; ++i) { fd->AddLine(ImVec2(ds.x * i / 3, 0), ImVec2(ds.x * i / 3, ds.y), gc); fd->AddLine(ImVec2(0, ds.y * i / 3), ImVec2(ds.x, ds.y * i / 3), gc); }
                if (g.cfg.guides == 3) { fd->AddLine(ImVec2(0, 0), ImVec2(ds.x, ds.y), gc); fd->AddLine(ImVec2(ds.x, 0), ImVec2(0, ds.y), gc); }
            }
        }
        if (g_toastT > 0 && !CaptureActive()) {
            ImVec2 ms = ImGui::CalcTextSize(g_toast.c_str());
            ImVec2 a((ds.x - ms.x) * 0.5f - 14, ds.y - 78), b2((ds.x + ms.x) * 0.5f + 14, ds.y - 78 + ms.y + 18);
            fd->AddRectFilled(a, b2, IM_COL32(20, 20, 22, 235), 8.0f);
            fd->AddText(ImVec2(a.x + 14, a.y + 9), IM_COL32(235, 235, 232, 255), g_toast.c_str());
        }
    }
    if (g_hideHints || g_render >= 2 || CaptureActive()) return;    // nothing on screen while rendering, or when the hints are switched off (F6)
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    char b[128];
    float y = 8;
    auto line = [&](ImU32 col, const char* t) { dl->AddText(ImVec2(10, y), col, t); y += ImGui::GetTextLineHeight() + 2; };
    if (g.capturing) line(IM_COL32(255, 70, 70, 255), "REC");
    if (g.recording) line(IM_COL32(255, 120, 60, 255), "RECORDING CAMERA  (F9 to stop)");
    if (g_render >= 2) line(IM_COL32(255, 70, 70, 255), "RENDERING PATH  (F5 cancels)");
    if (g_armT > 0) { std::snprintf(b, sizeof b, "NEW TAKE IN %.1f s  (scene reset)", g_armT); line(IM_COL32(255, 210, 90, 255), b); }
    if (g.freecam && !g.menuOpen) {
        std::snprintf(b, sizeof b, "free camera   speed %.0f   fov %.1f%s", g.cfg.move_speed, g.fov, g.playing ? "   [dolly]" : "");
        line(IM_COL32(220, 220, 215, 255), b);
        line(IM_COL32(120, 120, 118, 255), "K point   L undo   J preview   F5 render   N zombie   M move   U freeze   F exit");
    }
    if (g.frozen) line(IM_COL32(120, 190, 255, 255), "FROZEN");
}

// ---- 6x-style widgets
static int g_tab = 0;

static void AccentBar(float h = 12.0f) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 3), ImVec2(p.x + 3, p.y + 3 + h), ImGui::GetColorU32(g_accent), 1.5f);
}

static void CardBegin(const char* title, const char* id) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    AccentBar();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10);
    ImGui::TextUnformatted(title);
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 2));
}
static void CardEnd() { ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::Dummy(ImVec2(0, 2)); }

static bool Toggle(const char* label, bool* v) {
    ImGui::PushID(label);
    float fh = ImGui::GetFrameHeight(), h = fh * 0.62f, w = h * 1.9f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool hit = ImGui::InvisibleButton("##t", ImVec2(w, fh));
    if (hit) *v = !*v;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y0 = p.y + (fh - h) * 0.5f;
    ImU32 bg = *v ? ImGui::GetColorU32(ImVec4(0.92f, 0.92f, 0.92f, 1)) : IM_COL32(48, 48, 50, 255);
    dl->AddRectFilled(ImVec2(p.x, y0), ImVec2(p.x + w, y0 + h), bg, h * 0.5f);
    float cx = *v ? p.x + w - h * 0.5f : p.x + h * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, y0 + h * 0.5f), h * 0.36f, *v ? IM_COL32(20, 20, 20, 255) : IM_COL32(150, 150, 152, 255));
    ImGui::PopID();
    return hit;
}

static bool Seg(const char* id, const char* const* items, int n, int* cur) {
    bool ch = false;
    ImGui::PushID(id);
    float sp = 4.0f, w = (ImGui::GetContentRegionAvail().x - sp * (n - 1)) / n;
    for (int i = 0; i < n; ++i) {
        if (i) ImGui::SameLine(0, sp);
        bool sel = (*cur == i);
        if (sel) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.88f, 0.88f, 0.87f, 1)); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.88f, 0.87f, 1)); }
        if (ImGui::Button(items[i], ImVec2(w, 0)) && !sel) { *cur = i; ch = true; }
        if (sel) ImGui::PopStyleColor(3);
    }
    ImGui::PopID();
    return ch;
}

static bool Sl(const char* label, float* v, float lo, float hi, const char* fmt = "%.2f", ImGuiSliderFlags fl = 0) {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    char t[48]; std::snprintf(t, sizeof t, fmt, *v);
    float tw = ImGui::CalcTextSize(t).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - tw));
    ImGui::TextUnformatted(t);
    ImGui::SetNextItemWidth(-1);
    bool ch = ImGui::SliderFloat("##s", v, lo, hi, "", fl);
    ImGui::PopID();
    return ch;
}

static bool Btn(const char* l, bool primary = false, float w = -1) {
    if (primary) { ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.88f, 0.88f, 0.87f, 1)); ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.96f, 0.96f, 0.95f, 1)); }
    bool r = ImGui::Button(l, ImVec2(w, 0));
    if (primary) ImGui::PopStyleColor(3);
    return r;
}
static bool BtnDanger(const char* l) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.90f, 0.27f, 0.27f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.96f, 0.35f, 0.35f, 1));
    bool r = ImGui::Button(l, ImVec2(-1, 0));
    ImGui::PopStyleColor(2);
    return r;
}
static void Hint(const char* t) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]); ImGui::TextWrapped("%s", t); ImGui::PopStyleColor(); }

// two columns of cards, 6x page style
static void Cols2Begin(const char* id) { ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX); }
static void ColNext() { ImGui::TableNextColumn(); }
static void Cols2End() { ImGui::EndTable(); }

// world / effects state (applied through console commands when changed)
struct WorldFx {
    float sunColor[3] = {1.0f, 0.95f, 0.85f};
    float sunLight = 1.0f;
    float sunDir[3] = {0.0f, 0.0f, 0.0f};
    bool dof = false;
    float dofNearStart = 0, dofNearEnd = 10, dofFarStart = 300, dofFarEnd = 2000;
    float dofNearBlur = 6, dofFarBlur = 2;
    int dofTarget = 0;                       // 0 manual, 1 me, 2 selected zombie, 3 lock-on target, 4 all zombies
    float dofDepth = 120, dofRamp = 500, dofSpeed = 8, dofOffset = 0, dofDist = 0;
    int greenMode = 0;                       // 0 solid background, 1 depth key (everything behind the subject turns green)
    float greenCut = 1200, greenSoft = 60;
    bool greenFollow = true;
    bool fog = true;
    bool hudOff = false, gunOff = false, wire = false, green = false, crossOff = false, third = false;
    float greenCol[3] = {0.0f, 1.0f, 0.0f};
    bool grade = false;
    float bright = 0.0f, contrast = 1.0f, sat = 1.0f;
    float gameFov = 65.0f;
} g_fx;

static void SendCheats() { QueueCmd("sv_cheats 1"); }

// hides points, round counter and the rest of the HUD (cg_draw2D + the game's own hud_visible flag)
static void SetCleanScreen(bool off) {
    g_fx.hudOff = off;
    QueueCmd("cg_draw2D %d", off ? 0 : 1);
    ZmSend(off ? "hud 0" : "hud 1");
}

static void Toast(const std::string& t, double secs);
// ---- demo playback state (the demo page is further down; the take logic needs this)
static int g_demoCmd = 0;                       // 0 = demo <name>, 1 = playdemo <name>
static std::string g_demoMsg;
static bool g_demoActive = false;               // we started a demo and it is (probably) still running
static std::string g_demoName;
static int g_dState = 0;                        // 0 idle, 1 loading, 2 settling, 3 entering the free camera
static double g_dT = 0; static bool g_dSaw = false;
static std::string DemoPathFile() { return g.dir + "paths\\demo_" + g_demoName + ".bo2path"; }
static void DemoPlayName(const std::string& name) {
    if (g.frozen) ToggleFreeze();
    g_demoName = name;
    QueueCmd(g_demoCmd == 0 ? "demo %s" : "playdemo %s", name.c_str());
    g_demoActive = true; g_dState = 1; g_dT = 0; g_dSaw = false;
    g_demoMsg = "loading the demo...";
}
static void DemoStop() { QueueCmd("stopdemo"); g_demoActive = false; g_dState = 0; if (g.frozen) ToggleFreeze(); g_demoMsg = "demo stopped"; }
static void DemoTick(float dt) {
    if (g_dState == 0) return;
    g_dT += dt;
    bool ig = InGame();
    if (g_dState == 1) {
        if (!ig) g_dSaw = true;
        if ((g_dSaw && ig && g_dT > 1.0) || (g_dT > 15.0 && ig)) { g_dState = 2; g_dT = 0; }
        else if (g_dT > 60.0) { g_dState = 0; g_demoMsg = "the demo did not start - try the other play command"; }
    } else if (g_dState == 2) {
        if (!ig) { g_dState = 1; g_dT = 0; return; }
        if (g_dT > 2.0) { if (!g.freecam) { g.wantEnter = true; g.wantLeave = false; } g_dState = 3; g_dT = 0; }
    } else if (g_dState == 3) {
        if (g.freecam && g_dT > 0.6) {
            g_dState = 0;
            std::string pf = DemoPathFile();
            CamPath tmp;
            bool had = tmp.Load(pf) && tmp.keys.size() >= 2;
            if (had) { PathPush(); g.path = tmp; g.timeMs = 0; }
            if (g.cfg.demo_freeze_start && !g.frozen) ToggleFreeze();
            g_demoMsg = had ? "ready - your saved camera path for this demo is loaded" : "ready - fly with WASD, record a path with F9";
            Toast(g_demoMsg + (g.cfg.demo_freeze_start ? "  (frozen at the start; recording unfreezes it)" : ""), 8);
        } else if (g_dT > 12.0) { g_dState = 0; g_demoMsg = "could not enter the free camera - press F once the demo is on screen"; }
    }
}

// ---- free camera recording (F9): countdown -> samples the camera -> smooths + simplifies into editable keyframes
static std::vector<CamKey> g_recRaw;            // the last take exactly as flown
static std::string TakesDir() { return g.dir + "paths\\takes\\"; }
static void ProcessRec() {
    if (g_recRaw.size() < 2) return;
    std::vector<CamKey> s = CamPath::Smooth(g_recRaw, g.cfg.rec_smooth);
    std::vector<CamKey> k = CamPath::Simplify(s, g.cfg.rec_detail);
    PathPush();
    g.path.keys = k; g.path.interp = Interp::Smooth; g.timeMs = 0; g.playing = false;
    char b[160];
    std::snprintf(b, sizeof b, "take: %.1f s, %d camera samples became %d keyframes - press J to preview", g_recRaw.back().timeMs / 1000.0, (int)g_recRaw.size(), (int)k.size());
    g_pathMsg = b;
}
static void SaveTake() {
    CreateDirectoryA((g.dir + "paths").c_str(), nullptr);
    CreateDirectoryA(TakesDir().c_str(), nullptr);
    SYSTEMTIME t; GetLocalTime(&t);
    char n[64]; std::snprintf(n, sizeof n, "take_%02d%02d_%02d%02d%02d", t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    g.path.Save(TakesDir() + n + ".bo2path");
}
static void BeginRec() {
    g.recKeys.clear(); g.recMs = 0; g.recLastKeyMs = -1000; g.recording = true;
    if (g.frozen && g_demoActive) ToggleFreeze();                  // a demo that is frozen at its start begins now
    g_pathMsg = "recording... press F9 to stop";
}
static void ToggleRecord() {
    if (g.recording) {
        g.recording = false;
        if (g.recKeys.size() >= 6) { g_recRaw = g.recKeys; ProcessRec(); SaveTake(); }
        else g_pathMsg = "recording too short, nothing saved";
        return;
    }
    if (g_recArm > 0) { g_recArm = 0; g_pathMsg = "recording cancelled"; return; }
    if (!g.freecam) { g.wantEnter = true; g.wantLeave = false; }    // F9 does the whole thing: free camera on, countdown, record
    g.menuOpen = false;
    if (g.cfg.rec_countdown > 0.2f) g_recArm = g.cfg.rec_countdown; else BeginRec();
}

static void SetCamMode(int m) {      // 0 pov, 1 free, 2 dolly
    if (m == 0) { g.playing = false; g.wantLeave = true; g.wantEnter = false; return; }
    if (!g.freecam) { g.wantEnter = true; g.wantLeave = false; }
    if (m == 1) g.playing = false;
    else if (!g.path.keys.empty()) StartDolly();
}

// ---------------------------------------------------------------- pages ---

// ---- one-click workflow: render the camera path straight to video
static std::string g_renderMsg;
static bool g_autoDemo = true, g_autoDemoSent = false;

static void StartRender() {
    if (g.path.keys.size() < 2) { g_renderMsg = "add at least 2 camera points first (K)"; return; }
    g.loop = false; g.recording = false;
    if (!g.freecam) { g.wantEnter = true; g.wantLeave = false; }
    g_render = 1; g_renderMsg = FfmpegFound() ? "rendering..." : "rendering... (no video encoder yet - frames will be saved as pictures, install it on the Capture page)";
}
static void CancelRender() {
    if (g_render == 3 || g_render == 4) CaptureStop();
    StopDolly(); g_render = 0; g_renderMsg = "render cancelled";
}
static void Toast(const std::string& t, double secs);
static std::string TakeResultText();
static void UpdateRender() {
    if (g_render == 1 && g.freecam) { g.menuOpen = false; if (g.cfg.preroll > 0.05f) { g.timeMs = 0; g.wantSample = true; CaptureStart(); g_prerollT = g.cfg.preroll; g_render = 4; } else { StartDolly(); g_render = 2; } }
    else if (g_render == 2 && g.playing) { CaptureStart(); g_render = 3; }
    else if (g_render == 3 && !g.playing) { CaptureStop(); g_render = 0; Toast(TakeResultText(), 9); g_renderMsg = FfmpegFound() ? "done - the mp4 is in the captures folder" : "done - but no video encoder: install it on the Capture page"; }
}
// ---- one-key recording: countdown -> (path render | plain capture + zombie actions) -> toast with the file
static void Toast(const std::string& t, double secs) { g_toast = t; g_toastT = secs; }
static std::string TakeResultText() {
    std::string f = CaptureLastFile();
    char b[64]; std::snprintf(b, sizeof b, "%u frames", CaptureFrames());
    return "saved  " + (f.empty() ? std::string("(captures folder)") : f) + "   " + b;
}
// ---- shot list: each shot = camera path + zombie scene + look; "record all" renders them one after another
struct LookSet {
    int letterbox = 0, path_ease = 0, ramp_on = 0, blur = 1;
    float vignette = 0, grain = 0, exposure = 0, contrast = 0, saturation = 0, temperature = 0, tint = 0, chroma = 0, sharpen = 0, bloom = 0, streak = 0, shake = 0, shake_speed = 1, ramp_start = 1, ramp_end = 1;
};
static LookSet CaptureLook() {
    LookSet l; auto& c = g.cfg;
    l.letterbox = c.letterbox; l.path_ease = c.path_ease; l.ramp_on = c.ramp_on; l.blur = c.blur_samples;
    l.vignette = c.vignette; l.grain = c.grain; l.exposure = c.exposure; l.contrast = c.contrast; l.saturation = c.saturation;
    l.temperature = c.temperature; l.tint = c.tint; l.chroma = c.chroma; l.sharpen = c.sharpen; l.bloom = c.bloom; l.streak = c.streak;
    l.shake = c.shake; l.shake_speed = c.shake_speed; l.ramp_start = c.ramp_start; l.ramp_end = c.ramp_end;
    return l;
}
static void ApplyLook(const LookSet& l) {
    auto& c = g.cfg;
    c.letterbox = l.letterbox; c.path_ease = l.path_ease; c.ramp_on = l.ramp_on; c.blur_samples = l.blur;
    c.vignette = l.vignette; c.grain = l.grain; c.exposure = l.exposure; c.contrast = l.contrast; c.saturation = l.saturation;
    c.temperature = l.temperature; c.tint = l.tint; c.chroma = l.chroma; c.sharpen = l.sharpen; c.bloom = l.bloom; c.streak = l.streak;
    c.shake = l.shake; c.shake_speed = l.shake_speed; c.ramp_start = l.ramp_start; c.ramp_end = l.ramp_end;
}
struct Shot { std::string name; CamPath path; std::string scene; LookSet look; };
static std::vector<Shot> g_shots;
static int g_shotIdx = -1;          // shot being recorded by "record all", -1 = idle
static double g_shotWait = 0;       // seconds until its take starts (the scene needs a moment to spawn)
static bool g_shotArmed = false;    // its take has started
static void ShotLoad(int i) {
    Shot& s = g_shots[i];
    PathPush(); g.path = s.path; ApplyLook(s.look);
    if (!s.scene.empty()) ZmSceneLoad(s.scene);
    g.timeMs = 0; g.playing = false;
}
static void ShotNext();
static std::string ShotsFile() { return g.dir + "shots.txt"; }

static void StopTake() {
    CaptureStop(); g_plainTake = false; StopDolly(); ZmSeqStop();
    Toast(TakeResultText(), 9);
}
static void BeginTake() {
    if (g.path.keys.size() >= 2) StartRender();                       // path: camera + zombies + capture start together
    else {
        if (g.frozen && g_demoActive) ToggleFreeze();
        if (g_syncActions && ZmHasSeq()) ZmSeqStart();
        CaptureStart(); g_plainTake = true;
    }
}
static void RecordCore() {
    if (g_countdown > 0) { g_countdown = 0; Toast("cancelled", 3); return; }
    if (g_render) { CancelRender(); Toast("cancelled", 3); return; }
    if (CaptureActive()) { StopTake(); return; }
    g.menuOpen = false;
    float wait = g_countdownLen;
    if (g_syncActions && ZmSceneDirty()) { ZmSceneReset(); if (wait < 2.5f) wait = 2.5f; }   // zombies are put back first
    if (wait > 0.1f) g_countdown = wait; else BeginTake();
}
static void RecordPressed() {
    if (g_shotIdx >= 0) { g_shotIdx = -1; g_shotWait = 0; g_shotArmed = false; Toast("shot list stopped", 4); }
    RecordCore();
}
static void ShotNext() {
    while (g_shotIdx >= 0 && g_shotIdx < (int)g_shots.size() && g_shots[g_shotIdx].path.keys.size() < 2) ++g_shotIdx;   // shots without a path are skipped
    if (g_shotIdx < 0 || g_shotIdx >= (int)g_shots.size()) { if (g_shotIdx >= 0) Toast("all shots recorded - look in the captures folder", 10); g_shotIdx = -1; return; }
    ShotLoad(g_shotIdx);
    g_shotArmed = false; g_shotWait = 3.0;
    char b[96]; std::snprintf(b, sizeof b, "shot %d / %d  -  %s", g_shotIdx + 1, (int)g_shots.size(), g_shots[g_shotIdx].name.c_str());
    Toast(b, 3);
}
static void TickTake(float dt) {
    DemoTick(dt);
    if (g_recArm > 0) { g_recArm -= dt; if (g_recArm <= 0) { g_recArm = 0; BeginRec(); } }
    { static bool wasBusy = false; bool busy = CaptureActive() || g_render || g_countdown > 0;
      if (wasBusy && !busy && g_demoActive && g.cfg.demo_auto_rewind && g_dState == 0 && g_shotIdx < 0 && !g_demoName.empty()) DemoPlayName(g_demoName);
      wasBusy = busy; }
    if (g_shotIdx >= 0) {
        if (g_shotWait > 0) { g_shotWait -= dt; if (g_shotWait <= 0) { g_shotWait = 0; RecordCore(); g_shotArmed = true; } }
        else if (g_shotArmed && !g_countdown && !g_render && !CaptureActive()) { g_shotArmed = false; ++g_shotIdx; ShotNext(); }
    }
    if (g_render == 4) { g.timeMs = 0; g.wantSample = true; g_prerollT -= dt; if (g_prerollT <= 0) { StartDolly(); g_render = 2; } }
    static ULONGLONG t0 = 0; if (!t0) t0 = GetTickCount64();
    if (g_countdown > 0) { g_countdown -= dt; if (g_countdown <= 0) { g_countdown = 0; BeginTake(); } }
    if (g_toastT > 0) g_toastT -= dt;
    // the video encoder installs itself once, in the background
    if (GetTickCount64() - t0 > 12000 && g.cfg.capture_format == 0 && !FfmpegFound() && FfmpegInstallState() == 0) { FfmpegInstall(); Toast("installing the video encoder (one time, about a minute)...", 9); }
    static int lastFf = 0; int st = FfmpegInstallState();
    if (st != lastFf) { lastFf = st; if (st == 2) Toast("video encoder installed - recording makes mp4 now", 9); else if (st == 3) Toast("video encoder download failed - recording saves pictures instead", 9); }
}
static std::string AutoPathFile() { return g.dir + "paths\\autosave.bo2path"; }
static void AutoSavePath() {
    static double last = -1; static bool loaded = false;
    if (!loaded) { loaded = true; if (g.path.Load(AutoPathFile())) g_pathMsg = "loaded your last camera path"; last = g.path.Checksum(); return; }
    double cs = g.path.Checksum();
    if (std::fabs(cs - last) > 1e-6) {
        last = cs;
        CreateDirectoryA((g.dir + "paths").c_str(), nullptr);
        g.path.Save(AutoPathFile());
        if (g_demoActive && !g_demoName.empty() && g.path.keys.size() >= 2) g.path.Save(DemoPathFile());      // every demo remembers its own camera path
    }
}

static void PageCamera() {
    Cols2Begin("cam");
    ColNext();
    CardBegin("quick start", "c_quick");
    {
        int n = (int)g.path.keys.size();
        double dur = g.path.Duration() / 1000.0;
        ImGui::TextDisabled("1  fly with the free camera, or just press F9 to record a move");
        if (Btn(g.freecam ? "free camera is on  (F to leave)" : "enter free camera  (F)", !g.freecam)) { if (g.freecam) { g.wantLeave = true; g.wantEnter = false; g.playing = false; } else { g.wantEnter = true; g.wantLeave = false; } }
        ImGui::TextDisabled("2  F9 records your flight (3 s countdown) and turns it into keyframes, or add points with K");
        ImGui::BeginDisabled(!g.freecam);
        float w = (ImGui::GetContentRegionAvail().x - 8) / 3;
        if (Btn("add point (K)", false, w)) g.wantAddKey = true;
        ImGui::SameLine(); if (Btn("remove last (L)", false, w) && n) g.path.keys.pop_back();
        ImGui::SameLine(); if (Btn("clear", false, w)) { g.path.keys.clear(); g.playing = false; g.timeMs = 0; }
        ImGui::EndDisabled();
        ImGui::Text("%d point%s  -  %.1f s", n, n == 1 ? "" : "s", dur);
        ImGui::TextDisabled("3  zombie actions");
        Toggle("start the actions with the dolly", &g_syncActions);
        Hint("Set your zombies and action steps in the zombies tab. Dolly, preview and render all start the actions at the same moment, and put the zombies back for every take.");
        ImGui::TextDisabled("4  preview it");
        ImGui::BeginDisabled(n < 2 || !g.freecam);
        if (Btn(g.playing || g_armT > 0 ? "stop  (J)" : "rewind and play  (J)")) { if (g.playing || g_armT > 0) StopDolly(); else StartDolly(); }
        ImGui::EndDisabled();
        ImGui::TextDisabled("5  render it to video in one click");
        ImGui::BeginDisabled(n < 2);
        if (g_render == 0) { if (Btn("render path to video  (F5)", true)) StartRender(); }
        else if (BtnDanger("cancel render  (F5)")) CancelRender();
        ImGui::EndDisabled();
        if (!g_renderMsg.empty()) Hint(g_renderMsg.c_str());
        Toggle("hide the on-screen text  (F6)", &g_hideHints);
        Hint("Your path saves itself, and comes back next time.");
    }
    CardEnd();
    CardBegin("camera mode", "c_mode");
    {
        const char* m[] = {"pov", "free", "dolly"};
        int cur = !g.freecam ? 0 : (g.playing ? 2 : 1);
        if (Seg("mode", m, 3, &cur)) SetCamMode(cur);
        Hint("F free camera   G dolly   K add keyframe");
    }
    CardEnd();
    CardBegin("field of view", "c_fov");
    if (Sl("camera fov", &g.fov, 10.0f, 160.0f, "%.1f")) g.cam.target.fov = g.fov;
    {
        if (Btn("50", false, 60)) g.fov = 50; ImGui::SameLine();
        if (Btn("65", false, 60)) g.fov = 65; ImGui::SameLine();
        if (Btn("90", false, 60)) g.fov = 90; ImGui::SameLine();
        if (Btn("default", false, -1)) g.fov = g.cfg.default_fov;
    }
    ImGui::TextDisabled("lens (mm, full frame)");
    {
        static const int mm[] = {14, 24, 35, 50, 85, 135};
        for (int i = 0; i < 6; ++i) {
            char lb[16]; std::snprintf(lb, sizeof lb, "%dmm", mm[i]);
            if (i) ImGui::SameLine(0, 4);
            if (Btn(lb, false, 56)) { g.fov = Rad2Deg(2.0f * std::atan(36.0f / (2.0f * mm[i]))); g.cam.target.fov = g.fov; }
        }
    }
    CardEnd();
    CardBegin("free camera", "c_free");
    Sl("speed", &g.cfg.move_speed, 20.0f, 3000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    Sl("fast", &g.cfg.fast_mult, 1.0f, 10.0f, "%.1fx");
    Sl("slow", &g.cfg.slow_mult, 0.02f, 1.0f, "%.2fx");
    Sl("movement smoothing", &g.cfg.smooth_move, 0.0f, 30.0f, "%.1f");
    Sl("look smoothing", &g.cfg.smooth_look, 0.0f, 60.0f, "%.1f");
    Sl("roll speed", &g.cfg.roll_speed, 10.0f, 200.0f, "%.0f");
    Toggle("hide hud + gun in free camera", &g.cfg.hide_hud_in_free);
    if (Btn("reset tilt and fov")) { g.cam.target.roll = 0; g.fov = g.cfg.default_fov; }
    CardEnd();
    ColNext();
    CardBegin("lock on", "c_lock");
    {
        const char* lm[] = {"off", "point", "zombie", "me"};
        int cur = g.lockMode;
        ImGui::BeginDisabled(!g.freecam);
        if (Seg("lockm", lm, 4, &cur)) {
            if (cur == 2 && ZmSelectedId() <= 0) { cur = 0; g_pathMsg = "select a zombie on the zombies tab first"; }
            LockOn(cur, ZmSelectedId());
        }
        if (g.lockMode == 2 && Btn("aim at the zombie selected now")) LockOn(2, ZmSelectedId());
        if (g.lockMode) {
            Sl("aim smoothing", &g.lockSmooth, 1.0f, 40.0f, "%.1f");
            Sl("frame offset yaw", &g.lockOffYaw, -35.0f, 35.0f, "%.1f");
            Sl("frame offset pitch", &g.lockOffPitch, -25.0f, 25.0f, "%.1f");
            if (g.lockValid) ImGui::Text("target distance  %.0f", g.lockDist);
            else if (g.lockMode != 1) ImGui::TextDisabled(TrackState() == 3 ? "tracking is not available (start a new match with the new script)" : "waiting for the game to report the target...");
            if (!g.dolly) { if (Btn("dolly zoom from here", false)) StartDollyZoom(); }
            else if (BtnDanger("stop dolly zoom")) g.dolly = false;
        }
        ImGui::EndDisabled();
        Hint("The camera keeps the target in frame while you fly or play a path. Dolly zoom changes the lens as you move so the subject stays the same size and the background warps.");
    }
    CardEnd();
    CardBegin("camera rigs", "c_rigs");
    {
        static const char* rn[] = {"push in", "pull out", "crane up", "slide right", "slide left", "whip pan", "dutch push", "arc"};
        for (int i = 0; i < 8; ++i) {
            if (i % 2) ImGui::SameLine(0, 4);
            if (Btn(rn[i], false, (ImGui::GetContentRegionAvail().x - 4) * 0.5f)) ApplyRig(i);
        }
        Hint("Builds a ready-made move from where the camera is right now. It replaces the current path (undo with Ctrl+Z).");
    }
    CardEnd();
    CardBegin("camera effects", "c_fxcam");
    Sl("handheld shake", &g.cfg.shake, 0.0f, 3.0f, "%.2f");
    if (g.cfg.shake > 0.001f) Sl("shake speed", &g.cfg.shake_speed, 0.3f, 3.0f, "%.2f");
    ImGui::TextDisabled("orbit shot");
    ImGui::BeginDisabled(!g.freecam);
    static float orbR = 400;
    Sl("orbit distance", &orbR, 80.0f, 1500.0f, "%.0f");
    Sl("orbit speed (deg/s)", &g.orbitSpeed, -90.0f, 90.0f, "%.0f");
    if (g.orbit) Sl("orbit height", &g.orbitHeight, -300.0f, 400.0f, "%.0f");
    if (!g.orbit) { if (Btn("orbit the point in front of me", true)) StartOrbit(orbR); }
    else if (BtnDanger("stop orbit")) g.orbit = false;
    ImGui::EndDisabled();
    Hint("Orbit circles the spot you are looking at. Handheld shake only moves what the game shows, never your saved path.");
    CardEnd();
    CardBegin("speed ramp", "c_ramp");
    { bool r = g.cfg.ramp_on != 0; if (Toggle("ramp speed along the path", &r)) g.cfg.ramp_on = r ? 1 : 0; }
    ImGui::BeginDisabled(!g.cfg.ramp_on);
    Sl("speed at start", &g.cfg.ramp_start, 0.05f, 2.0f, "%.2fx");
    Sl("speed at end", &g.cfg.ramp_end, 0.05f, 2.0f, "%.2fx");
    ImGui::EndDisabled();
    Hint("Example: start 0.15x and end 1.0x = the shot opens in slow motion and eases into real speed. Works in the recorded video too.");
    CardEnd();
    CardBegin("time", "c_time");
    {
        float ts = g.frozen ? g.timescaleBeforeFreeze : g.timescale;
        if (Sl("timescale", &ts, 0.01f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic) && !g.frozen) SetTimescale(ts);
        const float p[] = {0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f};
        for (int i = 0; i < 6; ++i) { if (i) ImGui::SameLine(); char b[8]; std::snprintf(b, sizeof b, "%.2g", p[i]); if (Btn(b, false, 44)) SetTimescale(p[i]); }
        if (Btn(g.frozen ? "unfreeze game  (F4)" : "freeze game  (F4)", g.frozen)) ToggleFreeze();
        Toggle("path follows timescale", &g.cfg.path_follows_timescale);
    }
    CardEnd();
    CardBegin("recorded takes", "c_take");
    {
        if (Btn(g.recording ? "stop recording  (F9)" : g_recArm > 0 ? "cancel  (F9)" : "record a move  (F9)", g.recording || g_recArm > 0)) ToggleRecord();
        if (g.recording) ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "REC %.1f s", g.recMs / 1000.0);
        Hint("F9 turns on the free camera, counts down, records everything you fly, and stops on the next F9. It is smoothed and cut down to a few keyframes you can edit.");
        Sl("countdown (seconds)", &g.cfg.rec_countdown, 0.0f, 10.0f, "%.1f");
        bool rch = false;
        rch |= Sl("smoothing", &g.cfg.rec_smooth, 0.0f, 1.0f, "%.2f");
        rch |= Sl("keyframe detail", &g.cfg.rec_detail, 0.0f, 1.0f, "%.2f");
        if (rch && g_recRaw.size() >= 2 && !g.recording) ProcessRec();
        if (g_recRaw.size() >= 2) ImGui::TextDisabled("moving the sliders re-processes your last take (%d keyframes now)", (int)g.path.keys.size());
        static std::vector<std::string> takes; static double tl = -10;
        if (ImGui::GetTime() - tl > 3.0 || g.recording == false && ImGui::IsWindowAppearing()) {
            tl = ImGui::GetTime(); takes.clear();
            WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((TakesDir() + "*.bo2path").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) { do { std::string n = fd.cFileName; if (n.size() > 8) takes.push_back(n.substr(0, n.size() - 8)); } while (FindNextFileA(h, &fd)); FindClose(h); }
            std::sort(takes.begin(), takes.end(), std::greater<std::string>());
        }
        if (!takes.empty()) {
            ImGui::TextDisabled("every take is saved - pick the best one");
            int shown = 0;
            for (auto& t : takes) {
                if (shown++ >= 8) break;
                ImGui::PushID(t.c_str());
                float w2 = (ImGui::GetContentRegionAvail().x - 4) * 0.72f;
                if (Btn(t.c_str(), false, w2)) { PathPush(); if (g.path.Load(TakesDir() + t + ".bo2path")) { g.timeMs = 0; g_pathMsg = "loaded " + t; } }
                ImGui::SameLine(0, 4);
                if (BtnDanger("delete")) { DeleteFileA((TakesDir() + t + ".bo2path").c_str()); tl = -10; }
                ImGui::PopID();
            }
        }
    }
    CardEnd();
    CardBegin("camera path", "c_path");
    { const char* ez[] = {"linear", "ease in+out", "ease in", "ease out"}; ImGui::TextDisabled("path timing"); Seg("ease", ez, 4, &g.cfg.path_ease); }
    {
        if (Btn(g.recording ? "stop recording  (F9)" : "record free camera  (F9)", g.recording)) ToggleRecord();
        if (g.recording) ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "REC %.1f s  (%d samples)", g.recMs / 1000.0, (int)g.recKeys.size());
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##pn", g_pathName, sizeof g_pathName);
        float w = (ImGui::GetContentRegionAvail().x - 4) * 0.5f;
        if (Btn("save path", true, w)) { CreateDirectoryA((g.dir + "paths").c_str(), nullptr); g_pathMsg = g.path.Save(PathFile()) ? "saved " + PathFile() : "save failed"; }
        ImGui::SameLine(0, 4);
        if (Btn("load path", false, w)) { g_pathMsg = g.path.Load(PathFile()) ? "loaded " + PathFile() : "load failed (file missing?)"; g.timeMs = 0; }
        Toggle("loop", &g.loop);
        if (!g_pathMsg.empty()) Hint(g_pathMsg.c_str());
    }
    CardEnd();
    CardBegin("keyframes", "c_keys");
    {
        float w2 = (ImGui::GetContentRegionAvail().x - 4) * 0.5f;
        ImGui::BeginDisabled(g.undo.empty());
        if (Btn("undo  (Ctrl+Z)", false, w2)) PathUndo();
        ImGui::EndDisabled();
        ImGui::SameLine(0, 4);
        ImGui::BeginDisabled(g.redo.empty());
        if (Btn("redo  (Ctrl+Y)", false, w2)) PathRedo();
        ImGui::EndDisabled();
        double dur = g.path.Duration();
        if (g.path.keys.size() >= 2 && dur > 0) {
            ImGui::TextDisabled("scrub the shot");
            float t = (float)(g.timeMs / 1000.0), tmax = (float)(dur / 1000.0);
            ImGui::SetNextItemWidth(-1);
            ImGui::BeginDisabled(!g.freecam);
            if (ImGui::SliderFloat("##scrub", &t, 0.0f, tmax, "%.2f s")) { g.timeMs = t * 1000.0; g.playing = false; g.wantSample = true; }
            ImGui::EndDisabled();
            ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (double bt : g.beats) { float x = a.x + (float)(bt / dur) * (b.x - a.x); dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), IM_COL32(255, 200, 60, 200), 2.0f); }
            for (auto& k : g.path.keys) { float x = a.x + (float)(k.timeMs / dur) * (b.x - a.x); dl->AddCircleFilled(ImVec2(x, b.y + 2), 3.0f, IM_COL32(120, 200, 255, 230)); }
            if (!g.freecam) ImGui::TextDisabled("scrubbing needs the free camera");
            ImGui::TextDisabled("edit the shot at the scrub position");
            {
                float w2 = (ImGui::GetContentRegionAvail().x - 4) * 0.5f;
                if (Btn("add a point here", false, w2)) { CamPose p; if (g.path.Sample(g.timeMs, &p)) { CamKey nk; nk.pose = p; nk.timeMs = g.timeMs; PathPush(); g.path.keys.push_back(nk); g.path.Sort(); } }
                ImGui::SameLine(0, 4);
                if (Btn("even out timing", false, w2)) { PathPush(); g.path.EvenOut(); }
                if (Btn("cut everything before", false, w2)) { PathPush(); g.path.Trim(g.timeMs, dur); g.timeMs = 0; g.wantSample = true; }
                ImGui::SameLine(0, 4);
                if (Btn("cut everything after", false, w2)) { PathPush(); g.path.Trim(0, g.timeMs); g.wantSample = true; }
                if (Btn("reverse the shot", false, w2)) { PathPush(); g.path.Reverse(); }
                ImGui::SameLine(0, 4);
                static float newLen = 5.0f;
                if (Btn("set length", false, w2)) { PathPush(); g.path.StretchTo(newLen * 1000.0); }
                Sl("length (s)", &newLen, 0.5f, 60.0f, "%.1f");
            }
            ImGui::TextDisabled("beat markers (time the shot to your music)");
            float w3 = (ImGui::GetContentRegionAvail().x - 8) / 3;
            if (Btn("mark here", false, w3)) { g.beats.push_back(g.timeMs); std::sort(g.beats.begin(), g.beats.end()); }
            ImGui::SameLine(0, 4); if (Btn("clear marks", false, w3)) g.beats.clear();
            ImGui::SameLine(0, 4);
            static float bpm = 120;
            if (Btn("grid from bpm", false, w3)) { g.beats.clear(); for (double bt = 0; bt <= dur; bt += 60000.0 / std::max(30.0f, bpm)) g.beats.push_back(bt); }
            Sl("bpm", &bpm, 40.0f, 220.0f, "%.0f");
        }
        bool needSort = false;
        for (size_t i = 0; i < g.path.keys.size(); ++i) {
            CamKey& k = g.path.keys[i];
            ImGui::PushID((int)i);
            char hd[64]; std::snprintf(hd, sizeof hd, "point %d   %.2f s", (int)i + 1, k.timeMs / 1000.0);
            if (ImGui::TreeNode(hd)) {
                float ts = (float)(k.timeMs / 1000.0);
                if (ImGui::DragFloat("time (s)", &ts, 0.02f, 0.0f, 600.0f, "%.2f")) k.timeMs = ts * 1000.0;
                if (ImGui::IsItemActivated()) PathPush();
                if (ImGui::IsItemDeactivatedAfterEdit()) needSort = true;
                const char* ez[] = {"none", "in+out", "ease in", "ease out", "hold"};
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##ez", &k.ease, ez, 5)) {}
                if (ImGui::IsItemActivated()) PathPush();
                Sl("speed here", &k.speed, 0.05f, 2.0f, "%.2fx");
                if (ImGui::IsItemActivated()) PathPush();
                Sl("curve handle", &k.tan, 0.0f, 2.0f, "%.2f");
                if (ImGui::IsItemActivated()) PathPush();
                float w3 = (ImGui::GetContentRegionAvail().x - 8) / 3;
                if (Btn("go to", false, w3)) { g.timeMs = k.timeMs; g.playing = false; g.wantSample = true; }
                ImGui::SameLine(0, 4);
                if (Btn("use camera", false, w3) && g.freecam) { PathPush(); k.pose = g.cam.pose; k.pose.fov = g.fov; }
                ImGui::SameLine(0, 4);
                if (BtnDanger("delete")) { PathPush(); g.path.keys.erase(g.path.keys.begin() + i); ImGui::TreePop(); ImGui::PopID(); break; }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (needSort) g.path.Sort();
        Hint("Each point can have its own ease, its own game speed (slow motion there) and a curve handle: 0 = sharp corner, 1 = smooth, 2 = wide swing.");
    }
    CardEnd();
    CardBegin("position", "c_pos");
    ImGui::Text("x %.1f  y %.1f  z %.1f", g.cam.pose.pos.x, g.cam.pose.pos.y, g.cam.pose.pos.z);
    ImGui::Text("pitch %.1f  yaw %.1f  roll %.1f", g.cam.pose.pitch, g.cam.pose.yaw, g.cam.pose.roll);
    CardEnd();
    Cols2End();
}

static void PageWorld() {
    Cols2Begin("world");
    ColNext();
    CardBegin("map light", "w_sun");
    Toggle("live preview in game (recorded as you see it)", &g_liveGrade);
    Sl("brightness", &g_lightBright, -2.0f, 2.0f, "%.2f");
    ImGui::ColorEdit3("light colour", g_lightCol, ImGuiColorEditFlags_NoInputs);
    if (Btn("reset map light")) { g_lightBright = 0.0f; g_lightCol[0] = g_lightCol[1] = g_lightCol[2] = 1.0f; }
    Hint("This lights the whole picture, like a colour wash plus exposure. It is drawn by the tool, not the game, because the game ignores its own sun settings on this build. Weapon and screen text get the same light.");
    if (Btn("fix a black / broken map")) {
        SendCheats();
        QueueCmd("r_lightTweakSunLight 1"); QueueCmd("r_lightTweakSunColor 1 1 1");
        QueueCmd("r_filmUseTweaks 0"); QueueCmd("r_filmTweakEnable 0");
        QueueCmd("r_fog 1"); QueueCmd("r_fogTweak 0"); QueueCmd("r_dof_tweak 0"); QueueCmd("r_dof_enable 0");
    }
    CardEnd();
    ColNext();
    CardBegin("fog", "w_fog");
    if (Toggle("fog", &g_fx.fog)) { SendCheats(); QueueCmd("r_fog %d", g_fx.fog ? 1 : 0); }
    if (Btn("reset fog")) { g_fx.fog = true; SendCheats(); QueueCmd("r_fog 1"); }
    CardEnd();
    Cols2End();
}

static void DofApply(bool force) {
    static float last[6] = {-1, -1, -1, -1, -1, -1};
    float v[6] = {g_fx.dofNearStart, g_fx.dofNearEnd, g_fx.dofFarStart, g_fx.dofFarEnd, g_fx.dofNearBlur, g_fx.dofFarBlur};
    bool ch = force;
    for (int i = 0; i < 6; ++i) if (std::fabs(v[i] - last[i]) > (i < 4 ? 1.5f : 0.05f)) ch = true;
    if (!ch) return;
    for (int i = 0; i < 6; ++i) last[i] = v[i];
    // second route: the game script applies it with setdepthoffield, which does not depend on console dvars
    QueueCmd("set mvm_dof \"%.1f %.1f %.1f %.1f %.1f %.1f\"", v[0], v[1], v[2], v[3], v[4], v[5]);
}
static void DofOff() { QueueCmd("set mvm_dof \"\""); }
// live colour preview: brightness / contrast / desaturation go to the game as film tweaks (script side)
static void LiveGradeUpdate() {
    static std::string last = "?";
    std::string s;
    if (g_liveGrade) {
        float br = std::max(-0.5f, std::min(0.5f, g.cfg.exposure * 0.12f));
        float co = 1.0f + g.cfg.contrast * 0.9f;
        float ds = g.cfg.saturation < 0 ? std::min(1.0f, -g.cfg.saturation) : 0.0f;
        if (std::fabs(br) > 0.004f || std::fabs(co - 1.0f) > 0.004f || ds > 0.004f) {
            char b[96]; std::snprintf(b, sizeof b, "1 %.3f %.3f %.3f", br, co, ds); s = b;
        }
    }
    if (s != last) { last = s; QueueCmd("set mvm_film \"%s\"", s.c_str()); }
}
static void GreenApply() {
    SendCheats();
    if (!g_fx.green) { QueueCmd("r_fogTweak 0"); QueueCmd("r_fog 1"); return; }
    QueueCmd("r_clearColor %.2f %.2f %.2f 1", g_fx.greenCol[0], g_fx.greenCol[1], g_fx.greenCol[2]);
    QueueCmd("r_skipPvs 0");
    if (g_fx.greenMode == 0) { QueueCmd("r_fog 0"); return; }
    QueueCmd("r_fog 1"); QueueCmd("r_fogTweak 1");           // depth key: fog thickens to a solid colour behind the subject
    QueueCmd("r_fogColor %.2f %.2f %.2f", g_fx.greenCol[0], g_fx.greenCol[1], g_fx.greenCol[2]);
    QueueCmd("r_fogOpacity 1");
    QueueCmd("r_fogBaseDist %.0f", g_fx.greenCut);
    QueueCmd("r_fogHalfDist %.0f", std::max(1.0f, g_fx.greenSoft));
}
// every frame: depth of field follows the chosen subject, and the green screen follows the focus
static void DofUpdate(float dt) {
    static int lastWant = -999; static float cur = -1; static float lastCut = -1;
    int want = -1;
    if (g_fx.dof && g_fx.dofTarget == 1) want = 0;
    else if (g_fx.dof && g_fx.dofTarget == 2) want = ZmSelectedId() > 0 ? ZmSelectedId() : -1;
    else if (g_fx.dof && g_fx.dofTarget == 3) want = g.lockMode == 2 ? g.lockId : g.lockMode == 3 ? 0 : -1;
    else if (g_fx.dof && g_fx.dofTarget == 4) want = -2;
    if (want != lastWant) { lastWant = want; TrackSet(want, 1); }
    if (!g_fx.dof || g_fx.dofTarget == 0) { cur = -1; return; }
    Vec3 tp; float ex = 0; bool ok = false;
    if (g_fx.dofTarget == 3 && g.lockMode == 1) { tp = g.lockPoint; ok = true; }
    else if (want != -1) ok = TrackGet(&tp, 1, &ex);
    if (!ok) return;
    Vec3 cp = g.cam.pose.pos;
    float d = std::sqrt((tp.x - cp.x) * (tp.x - cp.x) + (tp.y - cp.y) * (tp.y - cp.y) + (tp.z - cp.z) * (tp.z - cp.z));
    if (cur < 0) cur = d; else cur += (d - cur) * SmoothFactor(g_fx.dofSpeed, dt);
    float F = std::max(5.0f, cur + g_fx.dofOffset);
    float depth = g_fx.dofDepth + (g_fx.dofTarget == 4 ? ex * 2.0f : 0.0f);
    g_fx.dofDist = F;
    g_fx.dofNearEnd = std::max(0.0f, F - depth * 0.5f);
    g_fx.dofNearStart = std::max(0.0f, g_fx.dofNearEnd - g_fx.dofRamp * 0.4f);
    g_fx.dofFarStart = F + depth * 0.5f;
    g_fx.dofFarEnd = g_fx.dofFarStart + g_fx.dofRamp;
    DofApply(false);
    if (g_fx.green && g_fx.greenMode == 1 && g_fx.greenFollow) {     // green screen: everything behind the focus subject
        float cut = g_fx.dofFarStart + 40.0f;
        if (std::fabs(cut - lastCut) > 8.0f) { lastCut = cut; g_fx.greenCut = cut; QueueCmd("r_fogBaseDist %.0f", cut); }
    }
}

static void PageShots() {
    Cols2Begin("shots");
    ColNext();
    CardBegin("scenes", "h_scene");
    {
        static char nm[48] = "scene 1";
        static std::vector<std::string> list; static bool first = true;
        if (first) { list = ZmSceneList(); first = false; }
        ImGui::SetNextItemWidth(-1); ImGui::InputText("##sn", nm, sizeof nm);
        float w2 = (ImGui::GetContentRegionAvail().x - 4) * 0.5f;
        if (Btn("save current zombies", true, w2)) { ZmSceneSave(nm); list = ZmSceneList(); g_pathMsg = std::string("scene saved: ") + nm; }
        ImGui::SameLine(0, 4);
        if (Btn("refresh list", false, w2)) list = ZmSceneList();
        for (auto& s : list) {
            ImGui::PushID(s.c_str());
            if (Btn(("load  " + s).c_str(), false, -1)) { g_pathMsg = ZmSceneLoad(s) ? "scene loaded: " + s : "scene load failed"; std::snprintf(nm, sizeof nm, "%s", s.c_str()); }
            ImGui::PopID();
        }
        if (list.empty()) Hint("No scenes yet. Set your zombies up on the zombies tab, then save them here.");
        else Hint("A scene remembers every zombie, its pose and your action steps. Zombies that were placed at the crosshair come back relative to where you look or stand when you load it.");
        if (!g_pathMsg.empty()) Hint(g_pathMsg.c_str());
    }
    CardEnd();
    ColNext();
    CardBegin("shot list", "h_shots");
    {
        static std::vector<std::string> scn; static bool f2 = true;
        if (f2) { scn = ZmSceneList(); f2 = false; }
        static char sname[48] = "shot 1";
        static int scenePick = 0;
        ImGui::SetNextItemWidth(-1); ImGui::InputText("##shn", sname, sizeof sname);
        std::vector<const char*> sc; sc.push_back("(no scene)");
        for (auto& s : scn) sc.push_back(s.c_str());
        if (scenePick >= (int)sc.size()) scenePick = 0;
        ImGui::SetNextItemWidth(-1); ImGui::Combo("##scn", &scenePick, sc.data(), (int)sc.size());
        float w3 = (ImGui::GetContentRegionAvail().x - 4) * 0.5f;
        if (Btn("add shot from now", true, w3)) {
            Shot s; s.name = sname; s.path = g.path; s.scene = scenePick > 0 ? scn[scenePick - 1] : ""; s.look = CaptureLook();
            g_shots.push_back(s);
            std::snprintf(sname, sizeof sname, "shot %d", (int)g_shots.size() + 1);
        }
        ImGui::SameLine(0, 4);
        if (Btn("refresh scenes", false, w3)) scn = ZmSceneList();
        ImGui::TextDisabled("a shot = your camera path + a scene + the look (grade, bars, blur...)");
        for (size_t i = 0; i < g_shots.size(); ++i) {
            ImGui::PushID((int)i);
            Shot& s = g_shots[i];
            ImGui::Text("%d  %s   %s%s  (%d pts)", (int)i + 1, s.name.c_str(), s.scene.empty() ? "" : "scene: ", s.scene.c_str(), (int)s.path.keys.size());
            float w4 = (ImGui::GetContentRegionAvail().x - 12) / 4;
            if (Btn("load", false, w4)) ShotLoad((int)i);
            ImGui::SameLine(0, 4);
            if (Btn("update", false, w4)) { s.path = g.path; s.look = CaptureLook(); }
            ImGui::SameLine(0, 4);
            if (Btn("up", false, w4) && i > 0) std::swap(g_shots[i], g_shots[i - 1]);
            ImGui::SameLine(0, 4);
            if (BtnDanger("x")) { g_shots.erase(g_shots.begin() + i); ImGui::PopID(); break; }
            ImGui::PopID();
        }
        bool busy = g_shotIdx >= 0;
        ImGui::BeginDisabled(g_shots.empty() && !busy);
        if (!busy) { if (Btn("record all shots", true)) { g_shotIdx = 0; ShotNext(); } }
        else if (BtnDanger("stop the shot list  (F5)")) { g_shotIdx = -1; g_shotWait = 0; g_shotArmed = false; if (g_countdown > 0) g_countdown = 0; if (g_render) CancelRender(); else if (CaptureActive()) StopTake(); }
        ImGui::EndDisabled();
        Hint("Loads each shot (path, zombies, look), records it as its own mp4, then does the next one. Press F5 to stop. Shots need a path with 2+ points.");
    }
    CardEnd();
    Cols2End();
}

static void PageEffects() {
    Cols2Begin("fx");
    ColNext();
    CardBegin("video look", "e_look");
    ImGui::TextDisabled("baked into the recorded video");
    const char* lb[] = {"no bars", "2.39", "2.00", "1.85"};
    Seg("letterbox", lb, 4, &g.cfg.letterbox);
    Sl("vignette", &g.cfg.vignette, 0.0f, 1.0f, "%.2f");
    Sl("film grain", &g.cfg.grain, 0.0f, 1.0f, "%.2f");
    Sl("bloom (glow)", &g.cfg.bloom, 0.0f, 1.0f, "%.2f");
    Sl("light streaks", &g.cfg.streak, 0.0f, 1.0f, "%.2f");
    Hint("Bars show on screen while you set it up. Vignette and grain only appear in the video.");
    CardEnd();
    CardBegin("color grade", "e_grade");
    ImGui::TextDisabled("one-click looks");
    {
        struct Look { const char* n; float ex, co, sa, te, ti, ch, sh, vi, gr; };
        static const Look L[] = {
            {"clean",    0,    0,    0,    0,    0,   0,    0,   0,    0},
            {"cinema",  -0.1f, 0.25f,-0.1f, 0.15f,-0.05f,0.12f,0.25f,0.45f,0.12f},
            {"teal+orange",0,  0.30f, 0.15f, 0.35f, 0.10f,0.10f,0.30f,0.35f,0.08f},
            {"cold",    -0.05f,0.20f,-0.20f,-0.45f,0,    0.08f,0.25f,0.40f,0.10f},
            {"vivid",    0.05f,0.30f, 0.45f, 0.05f,0,    0,    0.40f,0.20f,0},
            {"noir",    -0.1f, 0.55f,-1.0f,  0,    0,    0.15f,0.30f,0.65f,0.30f},
            {"vhs",      0.05f,-0.1f, 0.15f, 0.10f,0.15f,0.60f,0,    0.30f,0.35f},
        };
        for (int i = 0; i < 7; ++i) {
            if (i % 4) ImGui::SameLine(0, 4);
            if (Btn(L[i].n, false, 80)) {
                g.cfg.exposure = L[i].ex; g.cfg.contrast = L[i].co; g.cfg.saturation = L[i].sa; g.cfg.temperature = L[i].te;
                g.cfg.tint = L[i].ti; g.cfg.chroma = L[i].ch; g.cfg.sharpen = L[i].sh; g.cfg.vignette = L[i].vi; g.cfg.grain = L[i].gr;
            }
        }
    }
    Sl("exposure", &g.cfg.exposure, -2.0f, 2.0f, "%.2f");
    Sl("contrast", &g.cfg.contrast, -1.0f, 1.0f, "%.2f");
    Sl("saturation", &g.cfg.saturation, -1.0f, 1.0f, "%.2f");
    Sl("temperature", &g.cfg.temperature, -1.0f, 1.0f, "%.2f");
    Sl("tint", &g.cfg.tint, -1.0f, 1.0f, "%.2f");
    Sl("chromatic aberration", &g.cfg.chroma, 0.0f, 1.0f, "%.2f");
    Sl("sharpen", &g.cfg.sharpen, 0.0f, 1.0f, "%.2f");
    Toggle("live preview in game (recorded as you see it)", &g_liveGrade);
    Hint("Live preview shows the grade, temperature, tint, vignette, grain, chromatic aberration and sharpen on screen and in the video. Bloom and light streaks are only added to the saved video.");
    CardEnd();
    CardBegin("composition guides", "e_guides");
    { const char* gd[] = {"off", "thirds", "center", "thirds+diag"}; Seg("guides", gd, 4, &g.cfg.guides); }
    Hint("Guide lines are on screen only, never in the video.");
    CardEnd();
    CardBegin("depth of field", "e_dof");
    {
        bool on = Toggle("dof", &g_fx.dof);
        if (on) { SendCheats(); if (g_fx.dof) DofApply(true); else DofOff(); }
        ImGui::TextDisabled("focus on");
        const char* tg[] = {"manual", "me", "zombie", "lock-on", "all"};
        if (Seg("doft", tg, 5, &g_fx.dofTarget) && g_fx.dofTarget != 0 && !g_fx.dof) { g_fx.dof = true; SendCheats(); DofApply(true); }
        if (g_fx.dofTarget == 2) ImGui::TextDisabled(ZmSelectedId() > 0 ? "the zombie selected on the zombies tab" : "select a zombie on the zombies tab first");
        if (g_fx.dofTarget == 3 && !g.lockMode) ImGui::TextDisabled("turn on lock-on in the camera tab first");
        if (g_fx.dofTarget == 4) ImGui::TextDisabled("the sharp zone covers all your zombies");
        if (g_fx.dofTarget != 0 && !g.freecam) ImGui::TextDisabled("focus follows the free camera - enter it with F");
        if (g_fx.dofTarget != 0 && g_fx.dofDist > 0) ImGui::Text("focus distance  %.0f", g_fx.dofDist);
        if (g_fx.dofTarget != 0) {
            Sl("sharp zone", &g_fx.dofDepth, 10.0f, 1500.0f, "%.0f");
            Sl("softness distance", &g_fx.dofRamp, 50.0f, 3000.0f, "%.0f");
            Sl("focus speed", &g_fx.dofSpeed, 0.5f, 40.0f, "%.1f");
            Sl("focus offset", &g_fx.dofOffset, -400.0f, 400.0f, "%.0f");
        }
        bool bch = false;
        bch |= Sl("near blur", &g_fx.dofNearBlur, 0, 10, "%.1f");
        bch |= Sl("far blur", &g_fx.dofFarBlur, 0, 10, "%.1f");
        if (bch) { SendCheats(); DofApply(true); }
        if (ImGui::TreeNode("raw numbers (editing these switches to manual)")) {
            bool ch = false;
            ch |= Sl("near start", &g_fx.dofNearStart, 0, 500, "%.0f");
            ch |= Sl("near end", &g_fx.dofNearEnd, 0, 500, "%.0f");
            ch |= Sl("far start", &g_fx.dofFarStart, 0, 6000, "%.0f");
            ch |= Sl("far end", &g_fx.dofFarEnd, 0, 10000, "%.0f");
            if (ch) { g_fx.dofTarget = 0; SendCheats(); DofApply(true); }
            ImGui::TreePop();
        }
        if (Btn("reset dof")) { g_fx.dof = false; g_fx.dofTarget = 0; SendCheats(); DofOff(); }
    }
    CardEnd();
    ColNext();
    CardBegin("green screen", "e_green");
    {
        bool gs = Toggle("green screen", &g_fx.green);
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("color"); ImGui::SameLine();
        gs |= ImGui::ColorEdit3("##gs", g_fx.greenCol, ImGuiColorEditFlags_NoInputs);
        const char* gm[] = {"solid", "depth key"};
        gs |= Seg("gmode", gm, 2, &g_fx.greenMode);
        if (g_fx.greenMode == 1) {
            gs |= Sl("cut distance", &g_fx.greenCut, 100.0f, 6000.0f, "%.0f");
            gs |= Sl("edge softness", &g_fx.greenSoft, 1.0f, 400.0f, "%.0f");
            Toggle("follow the dof focus", &g_fx.greenFollow);
            Hint("Depth key: fog thickens into a solid colour behind your subject, so everything behind it becomes green. Pick a focus target in the dof card and the cut follows it. Some maps keep a bit of sky showing.");
        } else Hint("Solid: fog off and the background colour set. Works where nothing is drawn behind your subject.");
        if (gs) GreenApply();
    }
    CardEnd();
    CardBegin("models and game hud", "e_hud");
    if (Toggle("hud (points, round, ammo)", &g_fx.hudOff)) SetCleanScreen(g_fx.hudOff);
    ImGui::TextDisabled("(on = hidden, same as H)");
    if (Toggle("hide first person weapon", &g_fx.gunOff)) QueueCmd("cg_drawGun %d", g_fx.gunOff ? 0 : 1);
    if (Toggle("hide crosshair", &g_fx.crossOff)) QueueCmd("cg_drawCrosshair %d", g_fx.crossOff ? 0 : 1);
    if (Toggle("third person", &g_fx.third)) { SendCheats(); QueueCmd("cg_thirdPerson %d", g_fx.third ? 1 : 0); }
    if (Sl("game fov (your normal view)", &g_fx.gameFov, 65.0f, 120.0f, "%.0f")) {
        QueueCmd("cg_fov %.0f", g_fx.gameFov);
        g.cfg.restore_fov = (int)g_fx.gameFov;                    // leaving free camera / a render returns to this
        g.cfg.Save(g.dir + "settings.ini");                       // and it is applied again every time you inject
    }
    CardEnd();
    Cols2End();
}

static void OpenFolder(const std::string& path) {
    CreateDirectoryA(path.c_str(), nullptr);
    std::string cmd = "explorer.exe \"" + path + "\"";
    std::vector<char> cl(cmd.begin(), cmd.end()); cl.push_back(0);
    STARTUPINFOA si = {}; si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessA(nullptr, cl.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}

static void PageCapture() {
    Cols2Begin("cap");
    ColNext();
    CardBegin("video encoder", "p_enc");
    bool ff = FfmpegFound();
    int st = FfmpegInstallState();
    if (ff) Hint("ready - videos are saved as mp4");
    else if (st == 1) Hint("downloading the encoder, about 1 minute...");
    else if (st == 3) Hint("download failed - check your internet and try again");
    else Hint("not installed yet - without it every frame is saved as a huge picture file");
    if (!ff && st != 1 && Btn("install video encoder (one time)", true)) FfmpegInstall();
    CardEnd();
    CardBegin("smooth render", "p_smooth");
    Sl("pre-roll (seconds before the shot)", &g.cfg.preroll, 0.0f, 10.0f, "%.1f");
    if (Btn("save a still picture  (F8)", false)) { CaptureStill(); Toast("still saved in the captures folder", 4); }
    { bool sm = g.cfg.smooth_capture != 0; if (Toggle("fixed-step render (smooth video)", &sm)) g.cfg.smooth_capture = sm ? 1 : 0; }
    Hint("Every game frame becomes exactly one video frame and the game clock follows the video, so heavy scenes stay smooth. Recording takes as long as it takes, the video always plays at the right speed.");
    ImGui::BeginDisabled(!g.cfg.smooth_capture);
    ImGui::TextDisabled("motion blur (frames blended per video frame)");
    { int bi = g.cfg.blur_samples <= 1 ? 0 : g.cfg.blur_samples <= 2 ? 1 : g.cfg.blur_samples <= 4 ? 2 : g.cfg.blur_samples <= 8 ? 3 : 4;
      const char* bl[] = {"off", "2", "4", "8", "16"}; static const int bv[] = {1, 2, 4, 8, 16};
      if (Seg("blur", bl, 5, &bi)) g.cfg.blur_samples = bv[bi]; }
    ImGui::EndDisabled();
    Hint("More blur samples = smoother motion but the render takes that many times longer.");
    CardEnd();
    CardBegin("capture", "p_cap");
    int fps = g.cfg.capture_fps;
    float f = (float)fps; if (Sl("fps", &f, 24, 144, "%.0f")) g.cfg.capture_fps = (int)f;
    const char* fm[] = {"mp4 (ffmpeg)", "tga sequence"};
    Seg("fmt", fm, 2, &g.cfg.capture_format);
    float crf = (float)g.cfg.ffmpeg_crf; if (Sl("quality (crf, lower = better)", &crf, 0, 40, "%.0f")) g.cfg.ffmpeg_crf = (int)crf;
    Toggle("hide hud while capturing", &g.cfg.capture_hide_hud);
    Hint(("Output: " + g.dir + "captures\\").c_str());
    if (!CaptureActive()) { if (Btn("start capture", true)) CaptureStart(); }
    else                  { if (BtnDanger("stop capture")) CaptureStop(); }
    if (CaptureActive() || CaptureFrames() > 0) {
        char b[160];
        std::snprintf(b, sizeof b, "%u frames written, %u repeated (repeats mean the game could not keep up - lower fps or quality)", CaptureFrames(), CaptureDupes());
        Hint(b);
        if (!CaptureLastFile().empty()) Hint(("file: " + CaptureLastFile()).c_str());
    }
    if (Btn("open captures folder", false)) OpenFolder(g.dir + "captures\\");
    CardEnd();
    Cols2End();
}

static void PageSettings() {
    Cols2Begin("set");
    ColNext();
    CardBegin("appearance", "s_app");
    ImGui::TextUnformatted("accent color");
    for (int i = 0; i < 6; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, kAccents[i]); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccents[i]); ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccents[i]);
        if (ImGui::Button(i == g_accentIdx ? "o" : " ", ImVec2(26, 26))) { g_accentIdx = i; g_accent = kAccents[i]; StyleUi(); }
        ImGui::PopStyleColor(3);
        ImGui::PopID();
    }
    if (Sl("scale", &g.cfg.ui_scale, 0.7f, 2.0f, "%.2fx")) ImGui::GetStyle().FontScaleMain = g.cfg.ui_scale;
    Toggle("timeline while the menu is open", &g_showTimeline);
    Sl("record countdown (seconds)", &g_countdownLen, 0.0f, 10.0f, "%.0f");
    Toggle("hide the on-screen text  (F6)", &g_hideHints);
    CardEnd();
    CardBegin("files", "s_files");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##ff", g.cfg.ffmpeg_path, sizeof g.cfg.ffmpeg_path);
    Hint("ffmpeg path");
    if (Btn("save settings", true)) g_pathMsg = g.cfg.Save(g.dir + "settings.ini") ? "settings saved" : "settings save failed";
    if (!g_pathMsg.empty()) Hint(g_pathMsg.c_str());
    CardEnd();
    ColNext();
    CardBegin("shortcuts", "s_keys");
    const char* rows[][2] = {
        {"INSERT", "open / close the menu"}, {"F", "free camera"}, {"K / L", "add / remove last camera point"},
        {"J", "rewind and play the path"}, {"G", "dolly (play / stop)"}, {"F5", "render path / start-stop capture"},
        {"N", "spawn zombie at crosshair"}, {"M", "move selected zombie here"}, {"U", "freeze / unfreeze zombies"},
        {"WASD  E  Q/C", "move, up, down"}, {"Shift / Ctrl", "faster / slower"}, {"wheel", "fov"}, {"Ctrl + wheel", "camera speed"},
        {"Z / X, Alt+wheel", "roll"}, {"R", "reset tilt and fov"}, {"[ / ]", "slow motion - / +"},
        {"F4", "freeze game"}, {"F6", "hide / show the on-screen text"}, {"F9", "record free camera"}, {"H", "hide hud"}};
    for (auto& r : rows) { ImGui::TextUnformatted(r[0]); ImGui::SameLine(130); ImGui::TextDisabled("%s", r[1]); }
    CardEnd();
    Cols2End();
}


// ------------------------------------------------------------ demo page ---
// Uses the game's own demo recorder (record / stoprecord / playdemo / stopdemo, files demos\\<name>.dm_5).
struct DemoFile { std::string name, dir, map, mode, length; unsigned long long size = 0; FILETIME mt = {}; SYSTEMTIME st = {}; };
static std::vector<DemoFile> g_demos;
static int g_demoSel = -1;
static float g_demoSpeed = 1.0f;

static std::string JsonStr(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\":\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    size_t e = j.find('"', p);
    return e == std::string::npos ? "" : j.substr(p, e - p);
}
static std::string DemoDir() { return g.dir + "..\\demos\\"; }

static void RefreshDemos() {
    g_demos.clear();
    std::string dir = DemoDir();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*.demo").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            DemoFile f; f.name = fd.cFileName; f.dir = dir;
            f.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; f.mt = fd.ftLastWriteTime;
            FILETIME lt; FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lt); FileTimeToSystemTime(&lt, &f.st);
            size_t dot = f.name.rfind('.'); if (dot != std::string::npos) f.name = f.name.substr(0, dot);
            FILE* jf = nullptr; fopen_s(&jf, (dir + f.name + ".json").c_str(), "rb");
            if (jf) { char buf[1024]; size_t n = fread(buf, 1, sizeof buf - 1, jf); buf[n] = 0; fclose(jf);
                      std::string j(buf); f.map = JsonStr(j, "map"); f.mode = JsonStr(j, "gametype"); f.length = JsonStr(j, "length"); }
            g_demos.push_back(f);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(g_demos.begin(), g_demos.end(), [](const DemoFile& a, const DemoFile& b) { return CompareFileTime(&a.mt, &b.mt) > 0; });
    if (g_demoSel >= (int)g_demos.size()) g_demoSel = -1;
}

static void PageDemo() {
    static bool first = true;
    static double lastRefresh = 0;
    static char filter[48] = "";
    static int lastSel = -1;
    double now = ImGui::GetTime();
    if (first || now - lastRefresh > 5.0) { first = false; lastRefresh = now; RefreshDemos(); }
    Cols2Begin("demo");
    ColNext();
    CardBegin("film a demo", "d_film");
    {
        bool have = g_demoSel >= 0 && g_demoSel < (int)g_demos.size();
        if (!have) Hint("1  pick a demo on the right (the newest one is at the top)");
        else ImGui::TextWrapped("%s", g_demos[g_demoSel].name.c_str());
        ImGui::BeginDisabled(!have);
        if (Btn(g_dState ? "loading..." : "play + free camera", true)) { DemoPlayName(g_demos[g_demoSel].name); g.menuOpen = false; }
        ImGui::EndDisabled();
        Hint("Plays the demo, turns on the free camera by itself, and loads the camera path you made last time for this demo. Then fly, press F9 to record a move, F5 to render it.");
        { bool b = g.cfg.demo_freeze_start != 0; if (Toggle("start frozen at the first moment", &b)) g.cfg.demo_freeze_start = b ? 1 : 0; }
        Hint("The demo waits for you. F9 and F5 unfreeze it exactly when the recording or render starts, so every take begins at the same moment.");
        { bool b = g.cfg.demo_auto_rewind != 0; if (Toggle("restart the demo after every take", &b)) g.cfg.demo_auto_rewind = b ? 1 : 0; }
        if (!g_demoMsg.empty()) Hint(g_demoMsg.c_str());
        ImGui::Separator();
        ImGui::TextDisabled("%s   %s   %s", InGame() ? "in game" : "not in a match", g.freecam ? "free camera on" : "free camera off", g.frozen ? "frozen" : "running");
    }
    CardEnd();
    CardBegin("playback", "d_play");
    {
        float w2 = (ImGui::GetContentRegionAvail().x - 4) * 0.5f;
        if (Btn(g.frozen ? "resume" : "freeze  (F4)", false, w2)) ToggleFreeze();
        ImGui::SameLine(0, 4);
        ImGui::BeginDisabled(g_demoName.empty());
        if (Btn("restart", false, w2)) DemoPlayName(g_demoName);
        ImGui::EndDisabled();
        ImGui::TextDisabled("speed");
        static const float sp[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f};
        for (int i = 0; i < 5; ++i) {
            char lb[16]; std::snprintf(lb, sizeof lb, "%.2g", sp[i]);
            if (i) ImGui::SameLine(0, 4);
            if (Btn(lb, false, 52)) { if (g.frozen) ToggleFreeze(); g_demoSpeed = sp[i]; SetTimescale(sp[i]); }
        }
        if (Sl("speed", &g_demoSpeed, 0.05f, 3.0f, "%.2fx") && !g.frozen) SetTimescale(g_demoSpeed);
        if (BtnDanger("stop demo")) DemoStop();
        const char* cm[] = {"demo", "playdemo"};
        ImGui::TextDisabled("play command (if one does nothing, try the other)");
        Seg("pc", cm, 2, &g_demoCmd);
    }
    CardEnd();
    ColNext();
    CardBegin("demos", "d_list");
    {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##df", "search map or mode...", filter, sizeof filter);
        if (g_demos.empty()) Hint("no demos yet. The game records every match by itself - finish one and it shows up here.");
        std::string f = filter;
        for (auto& ch : f) ch = (char)std::tolower((unsigned char)ch);
        for (int i = 0; i < (int)g_demos.size(); ++i) {
            const DemoFile& d = g_demos[i];
            if (!f.empty()) {
                std::string hay = d.map + " " + d.mode + " " + d.name;
                for (auto& ch : hay) ch = (char)std::tolower((unsigned char)ch);
                if (hay.find(f) == std::string::npos) continue;
            }
            char l[220];
            std::snprintf(l, sizeof l, "%s  %s   %02d/%02d %02d:%02d   %s   %.1f MB##%d", d.map.c_str(), d.mode.c_str(), d.st.wMonth, d.st.wDay, d.st.wHour, d.st.wMinute, d.length.c_str(), (double)d.size / 1048576.0, i);
            if (ImGui::Selectable(l, g_demoSel == i)) g_demoSel = i;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { g_demoSel = i; DemoPlayName(d.name); g.menuOpen = false; }
        }
        if (Btn("refresh list")) RefreshDemos();
        Hint("Double click a demo to play it. Every demo remembers its own camera path.");
        Hint(("folder: " + DemoDir()).c_str());
    }
    CardEnd();
    Cols2End();
}

static void DrawTimeline() {
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    float h = 206.0f, w = ds.x;
    ImGui::SetNextWindowPos(ImVec2(0, ds.y - h));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##timeline", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    double dur = std::max(g.path.Duration(), 1000.0);
    bool canPlay = g.freecam && !g.path.keys.empty();
    ImGui::BeginDisabled(!canPlay);
    if (ImGui::Button(g.playing ? "||" : ">", ImVec2(30, 0))) { if (g.playing || g_armT > 0) StopDolly(); else StartDolly(); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    int tm = (int)(g.timeMs / 10), dm = (int)(dur / 10);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%02d:%02d.%02d", tm / 6000, (tm / 100) % 60, tm % 100);
    ImGui::SameLine(); ImGui::TextDisabled("/ %02d:%02d.%02d", dm / 6000, (dm / 100) % 60, dm % 100);
    ImGui::SameLine(0, 24);
    if (Btn("+ keyframe", true, 100)) if (g.freecam) g.wantAddKey = true;
    ImGui::SameLine();
    if (Btn("clear all (keyframes)", false, 160)) { g.path.keys.clear(); g.playing = false; g.timeMs = 0; }
    ImGui::SameLine();
    {
        int in = (int)g.path.interp; const char* nm[] = {"linear", "spline"};
        ImGui::SetNextItemWidth(90);
        if (ImGui::BeginCombo("##interp", nm[in])) { for (int i = 0; i < 2; ++i) if (ImGui::Selectable(nm[i], in == i)) g.path.interp = (Interp)i; ImGui::EndCombo(); }
    }
    ImGui::SameLine(0, 16);
    {
        float st = (float)(g.keyTimeStep / 1000.0); ImGui::SetNextItemWidth(90);
        if (ImGui::InputFloat("s / key", &st, 0, 0, "%.2f")) g.keyTimeStep = std::max(0.05f, st) * 1000.0;
    }
    // tracks
    const char* names[] = {"camera position", "camera rotation", "field of view", "velocity (timescale)", "zombie actions"};
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float labelW = 170.0f, rowH = 24.0f, x0 = p0.x + labelW, x1 = p0.x + (w - 20.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float st[16]; const char* sl[16]; int sn = ZmSeqTimes(st, sl, 16);
    for (int r = 0; r < 5; ++r) {
        float y = p0.y + 6 + r * (rowH + 4);
        dl->AddText(ImVec2(p0.x + 4, y + 4), IM_COL32(150, 150, 152, 255), names[r]);
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + rowH), IM_COL32(20, 20, 22, 255), 3.0f);
        if (r == 4) for (int i = 0; i < sn; ++i) {
            float sx = x0 + (st[i] * 1000.0f / (float)dur) * (x1 - x0);
            if (sx > x1) break;
            dl->AddRectFilled(ImVec2(sx, y + 3), ImVec2(sx + 3, y + rowH - 3), IM_COL32(120, 190, 255, 255));
            dl->AddText(ImVec2(sx + 6, y + 4), IM_COL32(170, 210, 255, 255), sl[i]);
        }
        if (r < 3) for (auto& k : g.path.keys) {
            float kx = x0 + (float)(k.timeMs / dur) * (x1 - x0);
            dl->AddQuadFilled(ImVec2(kx, y + 4), ImVec2(kx + 5, y + rowH * 0.5f), ImVec2(kx, y + rowH - 4), ImVec2(kx - 5, y + rowH * 0.5f), ImGui::GetColorU32(g_accent));
        }
    }
    float totalH = 6 + 5 * (rowH + 4);
    ImGui::SetCursorScreenPos(ImVec2(x0, p0.y));
    ImGui::InvisibleButton("##scrub", ImVec2(x1 - x0, totalH));
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(0) && !g.path.keys.empty()) {
        float t = (ImGui::GetIO().MousePos.x - x0) / (x1 - x0);
        g.timeMs = std::max(0.0f, std::min(1.0f, t)) * dur; g.playing = false; g.wantSample = g.freecam;
    }
    float hx = x0 + (float)(g.timeMs / dur) * (x1 - x0);
    dl->AddLine(ImVec2(hx, p0.y), ImVec2(hx, p0.y + totalH), IM_COL32(235, 235, 235, 255), 1.5f);
    dl->AddTriangleFilled(ImVec2(hx - 5, p0.y - 2), ImVec2(hx + 5, p0.y - 2), ImVec2(hx, p0.y + 6), IM_COL32(235, 235, 235, 255));
    ImGui::End();
    ImGui::PopStyleVar(2);
}

static void SidebarBtn(const char* label, const char* sub, int idx) {
    bool sel = (g_tab == idx);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(idx);
    if (ImGui::InvisibleButton("##s", ImVec2(56, 44))) g_tab = idx;
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (sel || hov) dl->AddRectFilled(p, ImVec2(p.x + 56, p.y + 44), sel ? IM_COL32(36, 36, 38, 255) : IM_COL32(24, 24, 26, 255), 6.0f);
    if (sel) dl->AddRectFilled(ImVec2(p.x - 6, p.y + 8), ImVec2(p.x - 3, p.y + 36), ImGui::GetColorU32(g_accent), 1.5f);
    ImU32 col = sel ? IM_COL32(240, 240, 240, 255) : IM_COL32(120, 120, 124, 255);
    // little glyph from the first letter, big, then the caption
    char gch[2] = {sub[0], 0};
    ImVec2 gs = ImGui::CalcTextSize(gch);
    dl->AddText(ImVec2(p.x + 28 - gs.x * 0.5f, p.y + 4), col, gch);
    ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + 28 - ls.x * 0.5f, p.y + 25), col, label);
    ImGui::PopID();
}

void DrawMenu() {
    ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(24, 14), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("##bo2mvm", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) { ImGui::End(); return; }
    // header
    ImGui::SetCursorPos(ImVec2(16, 12));
    ImGui::TextUnformatted("bo2mvm");
    ImGui::SameLine(); ImGui::TextDisabled("v1.0.0");
    {   // the one button that matters: record
        bool busy = CaptureActive() || g_render || g_countdown > 0;
        ImGui::SameLine(); ImGui::SetCursorPosX(150);
        ImGui::SetCursorPosY(7);
        ImGui::PushStyleColor(ImGuiCol_Button, busy ? ImVec4(0.75f, 0.18f, 0.18f, 1) : ImVec4(0.80f, 0.20f, 0.20f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.30f, 0.30f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.12f, 0.12f, 1));
        const char* lab = g_countdown > 0 ? "cancel  (F5)" : CaptureActive() || g_render ? "STOP  (F5)" : g.path.keys.size() >= 2 ? "RECORD CINEMATIC  (F5)" : "RECORD  (F5)";
        if (ImGui::Button(lab, ImVec2(190, 26))) RecordPressed();
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(g.path.keys.size() >= 2 ? "3-2-1, then the camera path, the zombie actions and the video start together" : "3-2-1, then it records the screen and starts your zombie actions. Press F5 again to stop.");
        ImGui::SetCursorPosY(12);
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 16 - ImGui::CalcTextSize(g.status.c_str()).x);
    ImGui::TextDisabled("%s", g.status.c_str());
    ImGui::SetCursorPos(ImVec2(0, 38));
    ImGui::Separator();
    if (!g.hooksOk) { ImGui::SetCursorPos(ImVec2(16, 52)); ImGui::TextWrapped("Hooks are off. See status above."); ImGui::End(); return; }

    ImGui::SetCursorPos(ImVec2(12, 50));
    ImGui::BeginGroup();
    SidebarBtn("demo", "D", 8);
    SidebarBtn("camera", "C", 0);
    SidebarBtn("zombies", "Z", 7);
    SidebarBtn("world", "W", 3);
    SidebarBtn("effects", "E", 4);
    SidebarBtn("capture", "R", 5);
    SidebarBtn("shots", "H", 9);
    SidebarBtn("settings", "S", 6);
    ImGui::EndGroup();

    ImGui::SetCursorPos(ImVec2(80, 39));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None);
    const char* titles[10] = {"camera", "time", "path", "world", "effects", "capture", "settings", "zombies", "demo", "shots"};
    ImGui::TextUnformatted(titles[g_tab < 10 ? g_tab : 0]);
    ImGui::Separator();
    switch (g_tab) {
        case 0: PageCamera(); break;
        case 3: PageWorld(); break;
        case 4: PageEffects(); break;
        case 5: PageCapture(); break;
        case 7: DrawZombiesTab(); break;
        case 8: PageDemo(); break;
        case 9: PageShots(); break;
        default: PageSettings(); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::End();
    if (g_showTimeline) DrawTimeline();
}

void HandleHotkeys() {
    static int dbg = 0;
    if (!g.hwnd || GetForegroundWindow() != g.hwnd) { if (dbg++ == 120) Log("hotkeys: game window not foreground (fg=%p hwnd=%p)", (void*)GetForegroundWindow(), (void*)g.hwnd); return; }
    if (KeyPressedEdge(g.cfg.menu_key)) { g.menuOpen = !g.menuOpen; Log("menu key pressed -> open=%d", (int)g.menuOpen); }
    if (!g.hooksOk) return;
    bool typing = ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;   // letter keys do nothing while typing in a text field
    if (typing) return;
    if (KeyPressedEdge(g.cfg.hud_key) && !g.menuOpen) SetCleanScreen(!g_fx.hudOff);
    if (KeyPressedEdge(g.cfg.record_key)) ToggleRecord();
    if (KeyPressedEdge(g.cfg.freecam_key) || KeyPressedEdge(VK_F2)) { Log("freecam key pressed (freecam=%d)", (int)g.freecam); if (g.freecam) { g.wantLeave = true; g.wantEnter = false; g.playing = false; } else { g.wantEnter = true; g.wantLeave = false; } }
    if (KeyPressedEdge(g.cfg.frozen_key)) ToggleFreeze();
    if (KeyPressedEdge(g.cfg.path_add_key) && g.freecam) g.wantAddKey = true;
    if (KeyPressedEdge(g.cfg.path_clear_key)) { g.path.keys.clear(); g.playing = false; g.timeMs = 0; }
    if (KeyPressedEdge(g.cfg.path_play_key) && g.freecam && !g.path.keys.empty()) {    // G = dolly
        if (g.playing || g_armT > 0) StopDolly(); else StartDolly();
    }
    AutoSavePath();
    if (KeyPressedEdge(VK_F6)) g_hideHints = !g_hideHints;
    if (KeyPressedEdge(VK_F8)) { CaptureStill(); Toast("still saved in the captures folder", 4); }
    if (g.freecam && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) { if (KeyPressedEdge('Z')) PathUndo(); if (KeyPressedEdge('Y')) PathRedo(); }
    TickDolly();
    {   // the game starts every match at its own fov (65, zoomed in): set your normal fov again whenever a match / demo begins
        static bool wasIn = false; static double fovT = -1; static bool init = false; static ULONGLONG fl = GetTickCount64();
        ULONGLONG fn = GetTickCount64(); double fdt = (double)(fn - fl) / 1000.0; fl = fn; if (fdt > 0.25) fdt = 0.25;
        if (!init) { init = true; g_fx.gameFov = (float)g.cfg.restore_fov; }
        bool ig = InGame();
        if (ig && !wasIn) fovT = 4.0;
        wasIn = ig;
        if (fovT > 0) {
            double prev = fovT; fovT -= fdt;
            bool fire = (prev > 3.0 && fovT <= 3.0) || (prev > 1.5 && fovT <= 1.5) || (prev > 0.0 && fovT <= 0.0);     // again after the game finishes spawning you
            if (fire && !g.freecam && g_render < 2) QueueCmd("cg_fov %d", g.cfg.restore_fov);
        }
    }
    UpdateRender();
    { static ULONGLONG lt = GetTickCount64(); ULONGLONG nt = GetTickCount64(); float d = (float)(nt - lt) / 1000.0f; lt = nt; if (d > 0.25f) d = 0; TickTake(d); DofUpdate(d); }
        if (KeyPressedEdge(VK_F5)) RecordPressed();          // F5: record (countdown, then the take) / stop / cancel
    if (g.freecam && KeyPressedEdge('J') && g.path.keys.size() >= 2) StartDolly();      // J: rewind and play
    if (g.freecam && KeyPressedEdge('L') && !g.path.keys.empty()) g.path.keys.pop_back();                      // L: remove last point
    if (KeyPressedEdge('N')) ZmQuickSpawn();            // N: spawn a zombie at the crosshair
    if (KeyPressedEdge('M')) ZmQuickMove();             // M: move the selected zombie to the crosshair
    if (KeyPressedEdge('U')) ZmQuickFreeze();           // U: freeze / unfreeze all zombies
    if (g.freecam && KeyPressedEdge('R')) { g.cam.target.roll = 0; g.fov = g.cfg.default_fov; }
    if (KeyPressedEdge(VK_OEM_4)) SetTimescale(std::max(0.01f, g.timescale * 0.5f));      // [  slower
    if (KeyPressedEdge(VK_OEM_6)) SetTimescale(std::min(4.0f, g.timescale * 2.0f));       // ]  faster
}

void CreateRtv() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
        g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
        bb->Release();
    }
}

bool InitImgui(IDXGISwapChain* sc) {
    static int tries = 0; if (tries++ < 3) Log("InitImgui try on swapchain %p", (void*)sc);
    DXGI_SWAP_CHAIN_DESC d;
    if (FAILED(sc->GetDesc(&d))) { Log("GetDesc failed"); return false; }
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev)) || !g_dev) return false;
    g_dev->GetImmediateContext(&g_ctx);
    g_sc = sc;
    g.hwnd = d.OutputWindow;
    CreateRtv();

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;   // menu stays usable by keyboard if the mouse is captured
    {
        const char* fonts[] = {"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf"};
        for (const char* f : fonts) {
            if (GetFileAttributesA(f) == INVALID_FILE_ATTRIBUTES) continue;
            ImFontConfig fc; fc.OversampleH = 3; fc.OversampleV = 2;
            if (io.Fonts->AddFontFromFileTTF(f, 16.0f, &fc)) break;
        }
    }
    StyleUi();
    ImGui_ImplWin32_Init(g.hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    g_origWndProc = (WNDPROC)SetWindowLongPtrA(g.hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
    g_imguiReady = true;
    Log("ImGui ready, hwnd=%p", (void*)g.hwnd);
    return true;
}

void DrawOverlayFrame(IDXGISwapChain* sc) {
    LiveFxPresent(sc);                 // live colour / vignette / grain pass
    CaptureFramePresent(sc);           // before the overlay is drawn
    ZombiesUpdate(sc);

    static unsigned pf = 0;
    if (++pf % 300 == 0 && pf < 300 * 40) CoreProbe();
    bool draw = false;
    {
        std::lock_guard<std::mutex> l(g.mx);
        HandleHotkeys();
        draw = true;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::GetIO().MouseDrawCursor = g.menuOpen;
        DrawHud();
        if (g.menuOpen) DrawMenu();
        ImGui::EndFrame();
    }
    if (draw && g_rtv) {
        ImGui::Render();
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (g_shuttingDown || (flags & DXGI_PRESENT_TEST)) return g_origPresent(sc, sync, flags);
    if (!g_imguiReady) {
        // only attach to the first (game) swap chain
        if (!InitImgui(sc)) return g_origPresent(sc, sync, flags);
    }
    if (sc != g_sc) {
        // the game recreated its swap chain / device (respawn, restart, vid_restart): rebind to the new one
        DXGI_SWAP_CHAIN_DESC nd;
        if (SUCCEEDED(sc->GetDesc(&nd)) && nd.OutputWindow == g.hwnd) {
            Log("swapchain changed (%p -> %p), rebinding overlay", (void*)g_sc, (void*)sc);
            ID3D11Device* nd_dev = nullptr;
            if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&nd_dev)) && nd_dev) {
                __try {
                    ImGui_ImplDX11_Shutdown();
                    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
                    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
                    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
                } __except (EXCEPTION_EXECUTE_HANDLER) { Log("exception while releasing old device objects"); }
                g_dev = nd_dev;
                g_dev->GetImmediateContext(&g_ctx);
                g_sc = sc;
                CreateRtv();
                ImGui_ImplDX11_Init(g_dev, g_ctx);
            }
        }
        if (sc != g_sc) return g_origPresent(sc, sync, flags);
    }

    static bool faulted = false;
    if (faulted) return g_origPresent(sc, sync, flags);
    __try {
        DrawOverlayFrame(sc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        Log("EXCEPTION in overlay draw - overlay disabled to protect the game");
    }
    return g_origPresent(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE hkResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    if (g_imguiReady && sc == g_sc) {
        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
        ImGui_ImplDX11_InvalidateDeviceObjects();
        HRESULT r = g_origResize(sc, n, w, h, f, fl);
        CreateRtv();
        ImGui_ImplDX11_CreateDeviceObjects();
        return r;
    }
    return g_origResize(sc, n, w, h, f, fl);
}

LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

// Create a throw-away swap chain only to read the IDXGISwapChain vtable.
bool FindVtable() {
    WNDCLASSEXA wc = {sizeof wc};
    wc.lpfnWndProc = DummyProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "bo2mvm_dummy";
    RegisterClassExA(&wc);
    HWND w = CreateWindowA(wc.lpszClassName, "d", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 100; sd.BufferDesc.Height = 100;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = w;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    bool ok = SUCCEEDED(hr) && sc;
    if (ok) g_vtable = *(void***)sc;
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(w);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return ok;
}

void PatchVtable(int idx, void* hook, void** orig) {
    DWORD old;
    VirtualProtect(&g_vtable[idx], sizeof(void*), PAGE_READWRITE, &old);
    *orig = g_vtable[idx];
    g_vtable[idx] = hook;
    VirtualProtect(&g_vtable[idx], sizeof(void*), old, &old);
}

}  // namespace

void UiCardBegin(const char* title, const char* id) { CardBegin(title, id); }
void UiCardEnd() { CardEnd(); }
void UiCols2Begin(const char* id) { Cols2Begin(id); }
void UiColNext() { ColNext(); }
void UiCols2End() { Cols2End(); }

bool OverlayInit() {
    if (!FindVtable()) { g.status = "overlay: could not create dummy DXGI device"; return false; }
    PatchVtable(8, (void*)hkPresent, (void**)&g_origPresent);       // IDXGISwapChain::Present
    PatchVtable(13, (void*)hkResize, (void**)&g_origResize);        // IDXGISwapChain::ResizeBuffers
    PatchIAT("user32.dll", "SetCursorPos", (void*)hkSetCursorPos, (void**)&g_origSetCursorPos);
    PatchIAT("user32.dll", "ClipCursor", (void*)hkClipCursor, (void**)&g_origClipCursor);
    bool gcp = PatchIAT("user32.dll", "GetCursorPos", (void*)hkGetCursorPos, (void**)&g_origGetCursorPos);
    Log("IAT GetCursorPos patched: %d, SetCursorPos patched: %d", (int)gcp, (int)(g_origSetCursorPos != nullptr));
    return true;
}

void OverlayShutdown() {
    g_shuttingDown = true;
    Sleep(100);
    if (g_vtable) {
        DWORD old;
        if (g_origPresent) { VirtualProtect(&g_vtable[8], sizeof(void*), PAGE_READWRITE, &old); g_vtable[8] = (void*)g_origPresent; VirtualProtect(&g_vtable[8], sizeof(void*), old, &old); }
        if (g_origResize)  { VirtualProtect(&g_vtable[13], sizeof(void*), PAGE_READWRITE, &old); g_vtable[13] = (void*)g_origResize; VirtualProtect(&g_vtable[13], sizeof(void*), old, &old); }
    }
    if (g_imguiReady) {
        if (g.hwnd && g_origWndProc) SetWindowLongPtrA(g.hwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (g_rtv) g_rtv->Release();
        if (g_ctx) g_ctx->Release();
        if (g_dev) g_dev->Release();
        g_imguiReady = false;
    }
}

}  // namespace mvm
