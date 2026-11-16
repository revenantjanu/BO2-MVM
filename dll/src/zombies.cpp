// zombies.cpp - zombie spawner + action editor.
// The game side is scripts/zm/mvm_zm.gsc; this file talks to it through the "mvm_cmd" dvar
// ("spawn / act / all / clear / hud / head" commands separated by '|').
#include "mvm.h"
#include "imgui.h"
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstring>
#include <d3d11.h>
#include <dxgi.h>

namespace mvm {
namespace {

struct ActDef { const char* label; const char* token; const char* group; const char* desc; };

// every token is handled in mvm_zm.gsc (mvm_do / mvm_mode)
const ActDef kActs[] = {
    {"Freeze",          "idle",         "Movement",        "Locks it in place, standing"},
    {"Walk",            "walk",         "Movement",        "Slow shamble toward you"},
    {"Run",             "run",          "Movement",        "Faster run cycle"},
    {"Sprint",          "sprint",       "Movement",        "Full sprint"},
    {"Super sprint",    "super_sprint", "Movement",        "Fastest cycle (Nacht/boss rounds)"},
    {"Crawl",           "stumpy",       "Movement",        "Legless crawler anim"},
    {"Headshot",        "k_head",       "Death by hit",    "Killed with a head hit"},
    {"Torso hit",       "k_torso",      "Death by hit",    "Killed with a chest hit"},
    {"Leg hit",         "k_legs",       "Death by hit",    "Killed with a leg hit"},
    {"Arm hit",         "k_arms",       "Death by hit",    "Killed with an arm hit"},
    {"Melee",           "k_melee",      "Death by hit",    "Killed by a melee hit"},
    {"Explosion",       "k_explode",    "Death by hit",    "Grenade blast, may gib"},
    {"Burn",            "k_burn",       "Death by hit",    "Fire death"},
    {"Standard death",  "d_normal",     "Death animation", "The normal zm_death anim"},
    {"Thundergun",      "d_jetgun",     "Death animation", "Thundergun / jetgun launch"},
    {"Jetgun sprinter", "d_jetgun_s",   "Death animation", "Jetgun death while sprinting"},
    {"Jetgun crawler",  "d_jetgun_c",   "Death animation", "Jetgun death as a crawler"},
    {"Remove",          "remove",       "Other",           "Deletes the zombie"},
};
constexpr int kActCount = (int)(sizeof kActs / sizeof kActs[0]);
constexpr int kMoveCount = 6;   // first six are the start behaviours

const char* kStartModes[] = {"idle", "walk", "run", "sprint", "super_sprint", "stumpy"};
const char* kStartLabels = "Stand still\0Walk\0Run\0Sprint\0Super sprint\0Crawl\0";


// ------------------------------------------------------------ pictograms ---
ImVec2 Rot(float x, float y, float deg) {
    float a = deg * 0.017453292f, c = std::cos(a), sn = std::sin(a);
    return ImVec2(x * c - y * sn, x * sn + y * c);
}

struct Pose { float tilt = 0, legA = 8, legB = -8, armA = 20, armB = 35; bool legless = false; };

// stick figure; local units: height ~1, pelvis at (0,0), faces right, +angles swing forward
void DrawFigure(ImDrawList* dl, ImVec2 o, float s, const Pose& p, ImU32 col, ImVec2* head, ImVec2* torso, ImVec2* leg, ImVec2* arm) {
    auto P = [&](float x, float y) { ImVec2 r = Rot(x * s, y * s, p.tilt); return ImVec2(o.x + r.x, o.y + r.y); };
    float th = std::max(2.0f, s * 0.07f);
    ImVec2 pel = P(0, 0), neck = P(0, -0.78), hd = P(0, -0.98), sh = P(0, -0.72);
    dl->AddLine(pel, neck, col, th);
    dl->AddCircleFilled(hd, s * 0.15f, col, 14);
    auto limb = [&](ImVec2 from, float lenN, float ang, float ox, float oy) {
        ImVec2 d = Rot(0, lenN * s, ang);           // pointing down, rotated by ang
        // build in figure space so the global tilt also applies
        ImVec2 loc0(ox, oy), loc1(ox - std::sin(ang * 0.017453292f) * -lenN * 0 + std::sin(ang * 0.017453292f) * lenN, oy + std::cos(ang * 0.017453292f) * lenN);
        (void)d; (void)from;
        ImVec2 a = P(loc0.x, loc0.y), b = P(loc1.x, loc1.y);
        dl->AddLine(a, b, col, th);
        return b;
    };
    ImVec2 la = limb(pel, p.legless ? 0.25f : 0.8f, p.legA, 0, 0);
    limb(pel, p.legless ? 0.2f : 0.8f, p.legB, 0, 0);
    ImVec2 aa = limb(sh, 0.55f, p.armA, 0, -0.72f);
    limb(sh, 0.55f, p.armB, 0, -0.72f);
    if (head) *head = hd;
    if (torso) *torso = P(0, -0.4f);
    if (leg) *leg = la;
    if (arm) *arm = aa;
    (void)neck;
}

void Ring(ImDrawList* dl, ImVec2 c, float r) {
    dl->AddCircle(c, r, IM_COL32(255, 60, 60, 255), 16, 2.0f);
    dl->AddCircleFilled(c, r * 0.25f, IM_COL32(255, 60, 60, 255), 8);
}
void Burst(ImDrawList* dl, ImVec2 c, float r, ImU32 col, int n = 10) {
    for (int i = 0; i < n; ++i) {
        float a = (float)i / n * 6.2831853f;
        float r1 = r * (i % 2 ? 0.45f : 0.7f);
        dl->AddLine(ImVec2(c.x + std::cos(a) * r1, c.y + std::sin(a) * r1), ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r), col, 2.0f);
    }
    dl->AddCircleFilled(c, r * 0.35f, col, 10);
}
void Flame(ImDrawList* dl, ImVec2 b, float h) {
    dl->AddTriangleFilled(ImVec2(b.x - h * 0.28f, b.y), ImVec2(b.x + h * 0.28f, b.y), ImVec2(b.x, b.y - h), IM_COL32(255, 140, 30, 255));
    dl->AddTriangleFilled(ImVec2(b.x - h * 0.14f, b.y), ImVec2(b.x + h * 0.14f, b.y), ImVec2(b.x, b.y - h * 0.6f), IM_COL32(255, 220, 80, 255));
}
void SpeedLines(ImDrawList* dl, ImVec2 c, float w, int n, ImU32 col) {
    for (int i = 0; i < n; ++i) {
        float y = c.y + (i - (n - 1) * 0.5f) * w * 0.22f;
        dl->AddLine(ImVec2(c.x - w * 0.5f, y), ImVec2(c.x - w * 0.15f, y), col, 2.0f);
    }
}

// draws the pictogram for an action token inside the square at `tl` of side `sz`
void DrawIcon(ImDrawList* dl, const char* tok, ImVec2 tl, float sz) {
    ImVec2 c(tl.x + sz * 0.5f, tl.y + sz * 0.5f);
    float s = sz * 0.36f;
    ImU32 body = IM_COL32(205, 210, 200, 255);
    ImU32 dim = IM_COL32(120, 125, 120, 255);
    ImVec2 o(c.x, c.y + sz * 0.14f);
    Pose p;
    ImVec2 head, torso, leg, arm;
    std::string t = tok;

    if (t == "idle")            { p.armA = 5; p.armB = -5; p.legA = 5; p.legB = -5; DrawFigure(dl, o, s, p, body, &head, &torso, &leg, &arm);
                                  dl->AddText(ImVec2(c.x + s * 0.5f, c.y - s * 1.15f), dim, "z z"); }
    else if (t == "walk")       { p.tilt = 6; p.legA = 22; p.legB = -18; p.armA = 80; p.armB = 95; DrawFigure(dl, o, s, p, body, &head, &torso, &leg, &arm); }
    else if (t == "run")        { p.tilt = 16; p.legA = 40; p.legB = -32; p.armA = 85; p.armB = 100; DrawFigure(dl, o, s, p, body, nullptr, nullptr, nullptr, nullptr);
                                  SpeedLines(dl, ImVec2(c.x - s * 0.5f, c.y), sz * 0.5f, 3, dim); }
    else if (t == "sprint")     { p.tilt = 26; p.legA = 55; p.legB = -45; p.armA = 95; p.armB = 110; DrawFigure(dl, o, s, p, body, nullptr, nullptr, nullptr, nullptr);
                                  SpeedLines(dl, ImVec2(c.x - s * 0.5f, c.y), sz * 0.6f, 4, dim); }
    else if (t == "super_sprint"){ p.tilt = 36; p.legA = 62; p.legB = -55; p.armA = 100; p.armB = 115; DrawFigure(dl, o, s, p, body, nullptr, nullptr, nullptr, nullptr);
                                  SpeedLines(dl, ImVec2(c.x - s * 0.5f, c.y), sz * 0.7f, 5, IM_COL32(255, 200, 60, 255)); }
    else if (t == "stumpy")     { p.tilt = 80; p.legless = true; p.legA = 5; p.legB = -5; p.armA = 150; p.armB = 160; DrawFigure(dl, ImVec2(c.x, c.y + sz * 0.22f), s, p, body, nullptr, nullptr, nullptr, nullptr);
                                  dl->AddLine(ImVec2(tl.x + sz * 0.12f, tl.y + sz * 0.82f), ImVec2(tl.x + sz * 0.88f, tl.y + sz * 0.82f), dim, 2.0f); }
    else if (t == "k_head")     { p.armA = 10; p.armB = -10; DrawFigure(dl, o, s, p, body, &head, nullptr, nullptr, nullptr); Ring(dl, head, s * 0.3f); }
    else if (t == "k_torso")    { p.armA = 10; p.armB = -10; DrawFigure(dl, o, s, p, body, nullptr, &torso, nullptr, nullptr); Ring(dl, torso, s * 0.3f); }
    else if (t == "k_legs")     { p.armA = 10; p.armB = -10; p.legA = 25; p.legB = -10; DrawFigure(dl, o, s, p, body, nullptr, nullptr, &leg, nullptr); Ring(dl, leg, s * 0.3f); }
    else if (t == "k_arms")     { p.armA = 50; p.armB = 15; DrawFigure(dl, o, s, p, body, nullptr, nullptr, nullptr, &arm); Ring(dl, arm, s * 0.3f); }
    else if (t == "k_melee")    { p.armA = 10; p.armB = -10; DrawFigure(dl, o, s, p, body, &head, &torso, nullptr, nullptr);
                                  Burst(dl, ImVec2(torso.x - s * 0.55f, torso.y), s * 0.35f, IM_COL32(255, 220, 90, 255), 8); }
    else if (t == "k_explode")  { Burst(dl, c, s * 0.95f, IM_COL32(255, 150, 40, 255), 12); p.tilt = 70; p.armA = 120; p.armB = 140; p.legA = 80; p.legB = -60;
                                  DrawFigure(dl, ImVec2(c.x + s * 0.3f, c.y + s * 0.25f), s * 0.6f, p, body, nullptr, nullptr, nullptr, nullptr); }
    else if (t == "k_burn")     { p.armA = 60; p.armB = 90; DrawFigure(dl, o, s, p, body, nullptr, nullptr, nullptr, nullptr);
                                  Flame(dl, ImVec2(c.x - s * 0.35f, o.y), s * 0.8f); Flame(dl, ImVec2(c.x + s * 0.35f, o.y - s * 0.2f), s * 0.7f); Flame(dl, ImVec2(c.x, o.y - s * 0.6f), s * 0.6f); }
    else if (t == "d_normal")   { p.tilt = -78; p.armA = 120; p.armB = 150; p.legA = 30; p.legB = 10; DrawFigure(dl, ImVec2(c.x, c.y + sz * 0.2f), s, p, body, nullptr, nullptr, nullptr, nullptr);
                                  dl->AddLine(ImVec2(tl.x + sz * 0.12f, tl.y + sz * 0.84f), ImVec2(tl.x + sz * 0.88f, tl.y + sz * 0.84f), dim, 2.0f); }
    else if (t == "d_jetgun" || t == "d_jetgun_s" || t == "d_jetgun_c") {
        bool cr = t == "d_jetgun_c";
        p.tilt = -50; p.armA = 140; p.armB = 160; p.legA = 60; p.legB = 35; p.legless = cr;
        DrawFigure(dl, ImVec2(c.x + s * 0.35f, c.y), s * 0.8f, p, body, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < 3; ++i) dl->AddCircle(ImVec2(tl.x + sz * 0.12f, c.y), s * (0.3f + i * 0.3f), IM_COL32(110, 190, 255, 255), 14, 2.0f);
        if (t == "d_jetgun_s") SpeedLines(dl, ImVec2(c.x + s * 0.1f, c.y - s * 0.6f), sz * 0.5f, 3, dim);
    }
    else if (t == "remove")     { dl->AddLine(ImVec2(c.x - s * 0.6f, c.y - s * 0.6f), ImVec2(c.x + s * 0.6f, c.y + s * 0.6f), IM_COL32(255, 80, 80, 255), 5.0f);
                                  dl->AddLine(ImVec2(c.x - s * 0.6f, c.y + s * 0.6f), ImVec2(c.x + s * 0.6f, c.y - s * 0.6f), IM_COL32(255, 80, 80, 255), 5.0f); }
}

// one selectable card (icon + name); returns true when clicked
bool ActCard(int idx, bool selected, float w, float h, bool showDesc) {
    ImGui::PushID(idx);
    ImVec2 tl = ImGui::GetCursorScreenPos();
    bool hit = ImGui::InvisibleButton("##card", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = selected ? IM_COL32(60, 95, 150, 255) : hov ? IM_COL32(50, 50, 52, 255) : IM_COL32(28, 28, 30, 255);
    dl->AddRectFilled(tl, ImVec2(tl.x + w, tl.y + h), bg, 4.0f);
    dl->AddRect(tl, ImVec2(tl.x + w, tl.y + h), selected ? IM_COL32(120, 170, 255, 255) : IM_COL32(70, 70, 72, 255), 4.0f);
    float is = std::min(w - 8, h - 24);
    DrawIcon(dl, kActs[idx].token, ImVec2(tl.x + (w - is) * 0.5f, tl.y + 4), is);
    dl->AddText(ImVec2(tl.x + 6, tl.y + h - 18), IM_COL32(235, 235, 235, 255), kActs[idx].label);
    if (hov && showDesc) ImGui::SetTooltip("%s", kActs[idx].desc);
    ImGui::PopID();
    return hit;
}

// button showing the current choice with a small icon; opens a popup grid of cards grouped by category
bool ActPicker(const char* id, int* act, int maxIdx) {
    bool changed = false;
    ImGui::PushID(id);
    ImVec2 tl = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##pick", ImVec2(210, 34))) ImGui::OpenPopup("grid");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    DrawIcon(dl, kActs[*act].token, ImVec2(tl.x + 2, tl.y + 1), 32);
    dl->AddText(ImVec2(tl.x + 40, tl.y + 9), IM_COL32(235, 235, 235, 255), kActs[*act].label);
    dl->AddText(ImVec2(tl.x + 190, tl.y + 9), IM_COL32(160, 160, 160, 255), "v");
    if (ImGui::BeginPopup("grid")) {
        const char* grp = nullptr;
        float cw = 124, ch = 112;
        int col = 0;
        for (int i = 0; i < maxIdx; ++i) {
            if (!grp || std::string(grp) != kActs[i].group) {
                grp = kActs[i].group;
                if (i) ImGui::NewLine();
                ImGui::TextColored(ImVec4(0.6f, 0.75f, 1, 1), "%s", grp);
                col = 0;
            }
            if (col) ImGui::SameLine();
            if (ActCard(i, i == *act, cw, ch, true)) { *act = i; changed = true; ImGui::CloseCurrentPopup(); }
            if (++col >= 5) col = 0;
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}


// -------------------------------------------------------- model discovery ---
// Zombie models are not listed anywhere in the scripts, so we read the names straight out of the
// running game: every loaded xmodel keeps its name string in memory ("c_zom_..."). Scan once per map.
std::mutex g_modMx;
std::vector<std::string> g_modBodies, g_modChars, g_modHeads, g_modExtras, g_modOther;
std::atomic<int> g_scanState{0};   // 0 not yet, 1 scanning, 2 done
std::string g_selBody, g_selHead, g_selExtra;  // empty = the map's normal zombie
std::string g_pBody, g_pHead, g_pExtra;        // the player's own character
char g_filter[48] = "";
bool g_showOther = false;
std::set<std::string> g_enabled;               // models the game script precached at this match start
bool g_enabledLoaded = false;
std::string g_mapName;
std::atomic<int> g_newUnlocks{0};              // models found that need one new match to unlock
std::atomic<int> g_verified{-1};               // models proven to be loaded in memory (-1 = not scanned)
std::string g_scanNote;

// ---- engine probe: lists the console command / dvar / function names in the running game that look demo related,
// so we can tell whether the engine has a usable demo recorder (written to bo2mvm_engine_probe.txt)
void ProbeEngine() {
    static std::atomic<bool> running{false};
    if (running.exchange(true)) return;
    std::set<std::string> found;
    SYSTEM_INFO si; GetSystemInfo(&si);
    const size_t CH = 1 << 20;
    std::vector<uint8_t> buf(CH + 160);
    const char* keys[] = {"demo", "record", "film", "theat", "replay", "playback", "snapshot_save", "cl_save", "savegame", "timedemo"};
    uint8_t* p = (uint8_t*)si.lpMinimumApplicationAddress;
    uint8_t* end = (uint8_t*)si.lpMaximumApplicationAddress;
    while (p < end && found.size() < 1500) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
        uint8_t* base = (uint8_t*)mbi.BaseAddress;
        size_t sz = mbi.RegionSize;
        bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && sz < (size_t)600 * 1024 * 1024;
        if (ok) {
            for (size_t off = 0; off < sz; off += CH) {
                size_t n = std::min(CH + 128, sz - off);
                SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), base + off, buf.data(), n, &got) || got < 8) continue;
                size_t lim = std::min<size_t>(got, CH);
                for (size_t i = 1; i + 4 < lim; ++i) {
                    uint8_t c0 = buf[i] | 0x20;
                    if (c0 != 'd' && c0 != 'r' && c0 != 'f' && c0 != 't' && c0 != 's' && c0 != 'c' && c0 != 'p') continue;
                    if (buf[i - 1] != 0) continue;                      // start of a string
                    size_t j = i;
                    while (j < got && j - i < 70 && buf[j] >= 0x20 && buf[j] < 0x7f) ++j;
                    if (j >= got || buf[j] != 0 || j - i < 4) continue;
                    std::string s((const char*)&buf[i], j - i), low = s;
                    for (auto& ch : low) ch = (char)std::tolower((unsigned char)ch);
                    for (const char* k : keys) if (low.find(k) != std::string::npos) { found.insert(s); break; }
                }
            }
        }
        p = base + sz;
        if (p <= base) break;
    }
    FILE* f = nullptr; fopen_s(&f, (g.dir + "bo2mvm_engine_probe.txt").c_str(), "w");
    if (f) { for (auto& s : found) fprintf(f, "%s\n", s.c_str()); fclose(f); }
    Log("engine probe wrote %d strings", (int)found.size());
    running = false;
}

// the map the current match runs on: Plutonium writes a small .json next to every match demo
std::string CurrentMap() {
    WIN32_FIND_DATAA fd;
    std::string dir = g.dir + "..\\demos\\";
    HANDLE h = FindFirstFileA((dir + "*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string best; FILETIME bt = {0, 0};
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &bt) > 0) { bt = fd.ftLastWriteTime; best = fd.cFileName; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    FILE* f = nullptr; fopen_s(&f, (dir + best).c_str(), "rb");
    if (!f) return "";
    char buf[2048]; size_t n = std::fread(buf, 1, sizeof buf - 1, f); std::fclose(f);
    buf[n] = 0;
    const char* k = std::strstr(buf, "\"map\":\"");
    if (!k) return "";
    k += 7;
    std::string m;
    while (*k && *k != '"' && m.size() < 40) m += *k++;
    return m;
}

void ReadNameList(const std::string& path, std::set<std::string>* out) {
    FILE* f = nullptr; fopen_s(&f, path.c_str(), "rb");
    if (!f) return;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty()) out->insert(l);
    }
    std::fclose(f);
}

// scripts\zm\mvm_models.gsc: precaches every model we found for each map we have seen, so the game script may use them
void WriteModelGsc() {
    std::map<std::string, std::vector<std::string>> maps;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g.dir + "models_*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string fn = fd.cFileName;
            std::string mp = fn.substr(7, fn.size() - 7 - 4);
            std::set<std::string> names;
            ReadNameList(g.dir + fn, &names);
            if (!names.empty()) maps[mp] = std::vector<std::string>(names.begin(), names.end());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::string out = "// generated by BO2MVM - lets the zombie script use models the game already has loaded.\n"
                      "// Safe to delete (or set the dvar mvm_models_off 1). The overlay writes it again.\n\n"
                      "init()\n{\n    if ( !isdefined( level.mvm_ok ) )\n        level.mvm_ok = [];\n\n"
                      "    if ( getdvar( \"mvm_models_off\" ) == \"1\" )\n        return;\n\n    map = getdvar( \"mapname\" );\n\n";
    std::string funcs;
    int fn = 0;
    for (auto& kv : maps) {
        out += "    if ( map == \"" + kv.first + "\" )\n    {\n";
        for (size_t i = 0; i < kv.second.size(); i += 30) {
            char nm[32]; std::snprintf(nm, sizeof nm, "mvm_m%d", fn++);
            out += std::string("        ") + nm + "();\n";
            funcs += std::string("\n") + nm + "()\n{\n";
            for (size_t j = i; j < kv.second.size() && j < i + 30; ++j) funcs += "    mvm_pc( \"" + kv.second[j] + "\" );\n";
            funcs += "}\n";
        }
        out += "    }\n\n";
    }
    out += "}\n" + funcs + "\nmvm_pc( n )\n{\n    precachemodel( n );\n    level.mvm_ok[n] = 1;\n}\n";
    CreateDirectoryA((g.dir + "..\\scripts").c_str(), nullptr);
    CreateDirectoryA((g.dir + "..\\scripts\\zm").c_str(), nullptr);
    FILE* f = nullptr; fopen_s(&f, (g.dir + "..\\scripts\\zm\\mvm_models.gsc").c_str(), "wb");
    if (f) { std::fwrite(out.data(), 1, out.size(), f); std::fclose(f); }
}

void ScanModels() {
    g_scanState = 1;
    g_mapName = CurrentMap();
    if (!g_enabledLoaded) {                    // what the script precached when this match started
        g_enabledLoaded = true;
        if (!g_mapName.empty()) ReadNameList(g.dir + "models_" + g_mapName + ".txt", &g_enabled);
    }
    const uint32_t K = 0xA5C3E179u;            // our own copies of addresses are stored scrambled so the scan never matches itself
    struct Hit { std::string name; uint32_t xa; };
    std::vector<Hit> hits;
    SYSTEM_INFO si; GetSystemInfo(&si);
    const size_t CH = 1 << 20;
    std::vector<uint8_t> buf(CH + 128);
    // pass 1: every "c_zom_..." string
    {
        uint8_t* p = (uint8_t*)si.lpMinimumApplicationAddress;
        uint8_t* end = (uint8_t*)si.lpMaximumApplicationAddress;
        while (p < end) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
            uint8_t* base = (uint8_t*)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;
            bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (ok) {
                for (size_t off = 0; off < sz; off += CH) {
                    size_t n = std::min(CH + 96, sz - off);
                    SIZE_T got = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), base + off, buf.data(), n, &got) || got < 8) continue;
                    size_t lim = std::min<size_t>(got, CH);
                    for (size_t i = 0; i + 6 < lim; ++i) {
                        if (buf[i] != 'c' || std::memcmp(&buf[i], "c_zom_", 6) != 0) continue;
                        if (i > 0 && buf[i - 1] != 0) continue;                 // must be the start of a string
                        size_t j = i;
                        while (j < got && ((buf[j] >= 'a' && buf[j] <= 'z') || (buf[j] >= '0' && buf[j] <= '9') || buf[j] == '_')) ++j;
                        if (j >= got || buf[j] != 0 || j - i < 9 || j - i > 70) continue;   // must end cleanly
                        hits.push_back({std::string((const char*)&buf[i], j - i), (uint32_t)(uintptr_t)(base + off + i) ^ K});
                    }
                }
            }
            p = base + sz;
            if (p <= base) break;
        }
    }
    // pass 2: a model that is really loaded has an asset header pointing at its name string. Names without any pointer
    // to them are only text (script strings, tables of other maps) and must not be precached.
    std::vector<uint32_t> keys;
    for (auto& h : hits) keys.push_back(h.xa);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::set<uint32_t> pointedAt;
    if (!keys.empty()) {
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (uint32_t k : keys) { uint32_t a = k ^ K; lo = std::min(lo, a); hi = std::max(hi, a); }
        uint8_t* p = (uint8_t*)si.lpMinimumApplicationAddress;
        uint8_t* end = (uint8_t*)si.lpMaximumApplicationAddress;
        while (p < end) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
            uint8_t* base = (uint8_t*)mbi.BaseAddress;
            size_t sz = mbi.RegionSize;
            bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (ok) {
                for (size_t off = 0; off < sz; off += CH) {
                    size_t n = std::min(CH, sz - off);
                    SIZE_T got = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), base + off, buf.data(), n, &got) || got < 4) continue;
                    const uint32_t* w = (const uint32_t*)buf.data();
                    size_t cnt = got / 4;
                    for (size_t i = 0; i < cnt; ++i) {
                        uint32_t v = w[i];
                        if (v < lo || v > hi) continue;
                        uint32_t x = v ^ K;
                        if (std::binary_search(keys.begin(), keys.end(), x)) pointedAt.insert(x);
                    }
                }
            }
            p = base + sz;
            if (p <= base) break;
        }
    }
    std::set<std::string> found, loaded;
    for (auto& h : hits) { found.insert(h.name); if (pointedAt.count(h.xa)) loaded.insert(h.name); }
    int nLoaded = (int)loaded.size();
    // if the pointer check found (almost) nothing the technique does not work on this build: do not trust it
    const std::set<std::string>& use = nLoaded >= 5 ? loaded : found;

    std::vector<std::string> b, c, hd, x, o;
    auto has = [](const std::string& n, const char* t) { return n.find(t) != std::string::npos; };
    auto ends = [](const std::string& n, const char* t) { size_t l = std::strlen(t); return n.size() >= l && n.compare(n.size() - l, l, t) == 0; };
    for (const auto& n : use) {
        if (has(n, "_g_") || has(n, "viewhands") || has(n, "spawn_pieces") || has(n, "ghost") || ends(n, "_mp")) o.push_back(n);   // gib pieces, first person arms
        else if (has(n, "hair") || has(n, "_hat") || has(n, "hat_") || has(n, "gear") || has(n, "glove") || has(n, "_arm_")) x.push_back(n);
        else if (has(n, "head")) hd.push_back(n);
        else if (ends(n, "_fb") || has(n, "_player_")) c.push_back(n);
        else if (has(n, "_body")) b.push_back(n);
        else o.push_back(n);
    }
    {
        std::lock_guard<std::mutex> l(g_modMx);
        g_modBodies = b; g_modChars = c; g_modHeads = hd; g_modExtras = x; g_modOther = o;
    }
    g_verified = nLoaded;
    Log("model scan: map '%s', %zu strings, %d proven loaded -> %zu bodies, %zu characters, %zu heads, %zu hats/hair, %zu other",
        g_mapName.c_str(), found.size(), nLoaded, b.size(), c.size(), hd.size(), x.size(), o.size());

    // raw list for debugging
    FILE* f = nullptr; fopen_s(&f, (g.dir + "zombie_models.txt").c_str(), "wb");
    if (f) { for (const auto& n : found) std::fprintf(f, "%s%s\n", n.c_str(), loaded.count(n) ? "" : "   (text only)"); std::fclose(f); }

    // unlock them for the next match
    if (nLoaded >= 5 && !g_mapName.empty()) {
        std::set<std::string> unlock;
        for (auto* v : {&b, &c, &hd, &x}) for (auto& n : *v) unlock.insert(n);
        FILE* mf = nullptr; fopen_s(&mf, (g.dir + "models_" + g_mapName + ".txt").c_str(), "wb");
        if (mf) { for (auto& n : unlock) std::fprintf(mf, "%s\n", n.c_str()); std::fclose(mf); }
        WriteModelGsc();
        int nu = 0;
        for (auto& n : unlock) if (!g_enabled.count(n)) ++nu;
        g_newUnlocks = nu;
        g_scanNote = nu ? "" : "all of them are unlocked";
    } else if (g_mapName.empty()) {
        g_scanNote = "could not tell which map this is";
    } else {
        g_scanNote = "could not verify which models are loaded - using what the map already has";
    }
    g_scanState = 2;
}

bool Unlocked(const std::string& n) { return n.empty() || g_enabled.count(n) > 0; }

// ------------------------------------------------------------ thumbnails ---
struct ThumbTex { ID3D11ShaderResourceView* srv = nullptr; bool tried = false; };
std::map<std::string, ThumbTex> g_thumbs;
ID3D11Device* g_dev = nullptr;
constexpr int kThumb = 128;

struct PrevReq { std::string key, token; };
std::vector<PrevReq> g_prevQ;
bool g_prevActive = false;
double g_prevT = 0;
PrevReq g_prevCur;

std::string ThumbKey(const std::string& body, const std::string& head, const std::string& extra) { return body + (head.empty() ? "" : "__" + head) + (extra.empty() ? "" : "__" + extra); }
std::string ThumbPath(const std::string& key) { return g.dir + "thumbs\\" + key + ".tga"; }

ID3D11ShaderResourceView* MakeTex(const uint8_t* rgba) {
    if (!g_dev) return nullptr;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = d.Height = kThumb; d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {rgba, (UINT)kThumb * 4, 0};
    ID3D11Texture2D* t = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, &sd, &t)) || !t) return nullptr;
    ID3D11ShaderResourceView* v = nullptr;
    g_dev->CreateShaderResourceView(t, nullptr, &v);
    t->Release();
    return v;
}

void SaveThumb(const std::string& key, const std::vector<uint8_t>& rgba) {
    CreateDirectoryA((g.dir + "thumbs").c_str(), nullptr);
    FILE* f = nullptr; fopen_s(&f, ThumbPath(key).c_str(), "wb");
    if (!f) return;
    uint8_t hdr[18] = {0};
    hdr[2] = 2; hdr[12] = kThumb & 255; hdr[13] = kThumb >> 8; hdr[14] = kThumb & 255; hdr[15] = kThumb >> 8; hdr[16] = 32; hdr[17] = 0x28;
    std::fwrite(hdr, 1, 18, f);
    std::vector<uint8_t> bgra(rgba);
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    std::fwrite(bgra.data(), 1, bgra.size(), f);
    std::fclose(f);
}

bool LoadThumbFile(const std::string& key, std::vector<uint8_t>* rgba) {
    FILE* f = nullptr; fopen_s(&f, ThumbPath(key).c_str(), "rb");
    if (!f) return false;
    uint8_t hdr[18];
    bool ok = std::fread(hdr, 1, 18, f) == 18 && hdr[2] == 2 && hdr[16] == 32 && (hdr[12] | hdr[13] << 8) == kThumb && (hdr[14] | hdr[15] << 8) == kThumb;
    if (ok) {
        rgba->resize((size_t)kThumb * kThumb * 4);
        ok = std::fread(rgba->data(), 1, rgba->size(), f) == rgba->size();
        if (ok) for (size_t i = 0; i + 3 < rgba->size(); i += 4) std::swap((*rgba)[i], (*rgba)[i + 2]);
    }
    std::fclose(f);
    return ok;
}

ID3D11ShaderResourceView* GetThumb(const std::string& key) {
    ThumbTex& t = g_thumbs[key];
    if (!t.srv && !t.tried && g_dev) {
        t.tried = true;
        std::vector<uint8_t> px;
        if (LoadThumbFile(key, &px)) t.srv = MakeTex(px.data());
    }
    return t.srv;
}

bool PreviewQueued(const std::string& key) {
    if (g_prevActive && g_prevCur.key == key) return true;
    for (const auto& r : g_prevQ) if (r.key == key) return true;
    return false;
}
void QueuePreview(const std::string& body, const std::string& head, const std::string& extra) {
    if (body.empty()) return;
    if (!Unlocked(body) || !Unlocked(head) || !Unlocked(extra)) return;     // the script cannot use it yet, the picture would be wrong
    std::string key = ThumbKey(body, head, extra);
    if (PreviewQueued(key)) return;
    g_prevQ.push_back({key, body + (head.empty() ? "" : "," + head) + (extra.empty() ? "" : "," + extra)});
}

std::string Short(const std::string& n) { return n.compare(0, 6, "c_zom_") == 0 ? n.substr(6) : n; }

// which selection a picker edits: set 0 = zombie, set 1 = my character
std::string& SelRef(int set, int kind) {
    if (set == 0) return kind == 0 ? g_selBody : kind == 1 ? g_selHead : g_selExtra;
    return kind == 0 ? g_pBody : kind == 1 ? g_pHead : g_pExtra;
}

// card with the preview picture (or a placeholder) and the model name
bool ModelCard(const std::string& name, int kind, int set, bool selected, const std::string& label) {
    float w = 108, h = 134;
    ImGui::PushID((name + "|" + std::to_string(kind) + "|" + std::to_string(set)).c_str());
    ImVec2 tl = ImGui::GetCursorScreenPos();
    bool hit = ImGui::InvisibleButton("##mc", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(tl, ImVec2(tl.x + w, tl.y + h), selected ? IM_COL32(60, 95, 150, 255) : hov ? IM_COL32(50, 50, 52, 255) : IM_COL32(28, 28, 30, 255), 4.0f);
    dl->AddRect(tl, ImVec2(tl.x + w, tl.y + h), selected ? IM_COL32(120, 170, 255, 255) : IM_COL32(70, 70, 72, 255), 4.0f);
    ImVec2 a(tl.x + 4, tl.y + 4), b(tl.x + w - 4, tl.y + w - 4);
    std::string& selBody = SelRef(set, 0); std::string& selHead = SelRef(set, 1); std::string& selExtra = SelRef(set, 2);
    std::string key = ThumbKey(kind == 0 ? name : selBody, kind == 1 ? name : selHead, kind == 2 ? name : selExtra);
    ID3D11ShaderResourceView* tex = (!name.empty() && !(kind != 0 && selBody.empty())) ? GetThumb(key) : nullptr;
    bool locked = !name.empty() && !Unlocked(name);
    if (tex) dl->AddImage((ImTextureID)(uintptr_t)tex, a, b);
    else {
        dl->AddRectFilled(a, b, IM_COL32(15, 15, 16, 255));
        const char* msg = name.empty() ? "none" : locked ? "new match needed" : PreviewQueued(key) ? "capturing..." : "no preview";
        dl->AddText(ImVec2(a.x + 8, a.y + (b.y - a.y) * 0.5f - 6), locked ? IM_COL32(255, 190, 90, 255) : IM_COL32(150, 150, 150, 255), msg);
    }
    dl->PushClipRect(ImVec2(tl.x, tl.y + w - 2), ImVec2(tl.x + w, tl.y + h), true);
    dl->AddText(ImVec2(tl.x + 5, tl.y + w + 2), IM_COL32(235, 235, 235, 255), label.c_str());
    dl->PopClipRect();
    if (hov) ImGui::SetTooltip("%s", name.empty() ? "Map's normal zombie / nothing" : name.c_str());
    ImGui::PopID();
    return hit;
}

void ModelList(const char* popupId, int kind, int set) {
    ImGui::SetNextWindowSize(ImVec2(660, 540), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup(popupId)) return;
    std::vector<std::string> list;
    {
        std::lock_guard<std::mutex> l(g_modMx);
        if (kind == 0) {
            list = set == 1 ? g_modChars : g_modBodies;
            const auto& more = set == 1 ? g_modBodies : g_modChars;
            list.insert(list.end(), more.begin(), more.end());
        } else list = kind == 1 ? g_modHeads : g_modExtras;
        if (g_showOther) list.insert(list.end(), g_modOther.begin(), g_modOther.end());
    }
    std::string& sel = SelRef(set, kind);
    std::string& selBody = SelRef(set, 0);
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##flt", "search...", g_filter, sizeof g_filter);
    ImGui::SameLine();
    ImGui::Checkbox("show gibs/hands/other", &g_showOther);
    std::vector<std::string> shown;
    for (const auto& n : list)
        if (!g_filter[0] || n.find(g_filter) != std::string::npos) shown.push_back(n);
    ImGui::SameLine();
    if (ImGui::Button("Capture previews of all shown")) {
        for (const auto& n : shown)
            QueuePreview(kind == 0 ? n : selBody, kind == 1 ? n : SelRef(set, 1), kind == 2 ? n : SelRef(set, 2));
    }
    // pictures for everything shown that does not have one yet, taken in the background while this window is open
    if (g_scanState.load() == 2 && !(kind != 0 && selBody.empty())) {
        static std::set<std::string> asked;
        int budget = 40;
        for (const auto& n : shown) {
            if (budget <= 0) break;
            std::string k = ThumbKey(kind == 0 ? n : selBody, kind == 1 ? n : SelRef(set, 1), kind == 2 ? n : SelRef(set, 2));
            if (asked.count(k)) continue;
            asked.insert(k);
            if (!GetThumb(k) && Unlocked(n)) { QueuePreview(kind == 0 ? n : selBody, kind == 1 ? n : SelRef(set, 1), kind == 2 ? n : SelRef(set, 2)); --budget; }
        }
    }
    if (kind != 0 && selBody.empty()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Pick a body first, heads and hats only work together with a body.");
    ImGui::TextDisabled("Orange = needs one new match to unlock. Keep the menu open while previews are taken.");
    ImGui::Separator();
    ImGui::BeginChild("mlist", ImVec2(0, 0), false);
    float availW = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)(availW / 114.0f));
    int c = 0;
    if (ModelCard("", kind, set, sel.empty(), kind == 0 ? (set == 1 ? "My normal one" : "Map default") : "None")) { sel = ""; if (kind == 0) { SelRef(set, 1) = ""; SelRef(set, 2) = ""; } ImGui::CloseCurrentPopup(); }
    ++c;
    for (const auto& n : shown) {
        if (c % cols) ImGui::SameLine();
        if (ModelCard(n, kind, set, sel == n, Short(n))) { sel = n; ImGui::CloseCurrentPopup(); }
        ++c;
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

std::string TokenOf(const std::string& body, const std::string& head, const std::string& extra) {
    if (body.empty()) return "default";
    return body + (head.empty() ? "" : "," + head) + (extra.empty() ? "" : "," + extra);
}
std::string ModelToken() { return TokenOf(g_selBody, g_selHead, g_selExtra); }

// the three pickers + big preview. set 0 = zombie, set 1 = my character
void PickerRow(int set) {
    std::string& body = SelRef(set, 0); std::string& head = SelRef(set, 1); std::string& extra = SelRef(set, 2);
    bool ready = g_scanState.load() == 2;
    ImGui::BeginDisabled(!ready);
    std::string pb = std::string(set ? "pick_pbody" : "pick_body"), ph = std::string(set ? "pick_phead" : "pick_head"), pe = std::string(set ? "pick_pextra" : "pick_extra");
    if (ImGui::Button(body.empty() ? (set ? "Body: my normal one" : "Body: map default") : ("Body: " + Short(body)).c_str(), ImVec2(200, 0))) ImGui::OpenPopup(pb.c_str());
    ImGui::SameLine();
    if (ImGui::Button(head.empty() ? "Head: default" : ("Head: " + Short(head)).c_str(), ImVec2(200, 0))) ImGui::OpenPopup(ph.c_str());
    ImGui::SameLine();
    if (ImGui::Button(extra.empty() ? "Hat/hair: none" : ("Hat/hair: " + Short(extra)).c_str(), ImVec2(200, 0))) ImGui::OpenPopup(pe.c_str());
    ImGui::EndDisabled();
    ModelList(pb.c_str(), 0, set);
    ModelList(ph.c_str(), 1, set);
    ModelList(pe.c_str(), 2, set);
    if (!body.empty()) {
        std::string key = ThumbKey(body, head, extra);
        ImGui::PushID(set);
        ID3D11ShaderResourceView* t = GetThumb(key);
        if (t) { ImGui::Image((ImTextureID)(uintptr_t)t, ImVec2(96, 96)); ImGui::SameLine(); }
        ImGui::BeginGroup();
        bool ok = Unlocked(body) && (head.empty() || Unlocked(head)) && (extra.empty() || Unlocked(extra));
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button("Capture preview of this")) QueuePreview(body, head, extra);
        ImGui::EndDisabled();
        if (!ok) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "needs one new match to unlock");
        else if (!g_prevQ.empty() || g_prevActive) ImGui::TextDisabled("%d preview(s) queued", (int)g_prevQ.size() + (g_prevActive ? 1 : 0));
        ImGui::EndGroup();
        ImGui::PopID();
    }
}

void ModelUI() {
    ImGui::Text("Model");
    int st = g_scanState.load();
    if (ImGui::Button(st == 1 ? "Scanning..." : st == 2 ? "Rescan loaded models" : "Find zombie models", ImVec2(190, 0)) && st != 1)
        std::thread([] { ScanModels(); }).detach();
    ImGui::SameLine();
    {
        std::lock_guard<std::mutex> l(g_modMx);
        if (st == 2) ImGui::TextDisabled("%d bodies, %d characters, %d heads", (int)g_modBodies.size(), (int)g_modChars.size(), (int)g_modHeads.size());
        else ImGui::TextDisabled("reads the models the game has loaded");
    }
    if (st == 2 && g_newUnlocks > 0)
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%d models are unlocked for your NEXT match. Start a new match once and they all work.", g_newUnlocks.load());
    else if (st == 2 && !g_scanNote.empty()) ImGui::TextDisabled("%s", g_scanNote.c_str());
    PickerRow(0);
}

struct Step { int act = 1; float wait = 3.0f; };   // do `act`, then wait `wait` seconds before the next step

std::string g_pending;          // commands waiting to be sent
double g_lastSend = -10;
std::vector<int> g_ids;         // zombies we spawned
int g_nextId = 1;
int g_sel = -1;                 // index into g_ids
std::vector<Step> g_seq = { {1, 3.0f}, {7, 1.0f} };
bool g_run = false;
double g_runDelay = 0;            // seconds until a queued run starts (after a scene reset)
std::vector<std::string> g_spawnLog;   // spawn commands of the current scene, replayed to reset it between takes
bool g_dirty = false;                // a sequence has run since the scene was spawned
int g_stepIdx = 0;
double g_stepT = 0;
int g_target = 0;               // 0 = selected zombie, 1 = all zombies
int g_headId = 0;
char g_model[96] = "default";
int g_count = 1;
float g_dist = 350, g_spacing = 90;
int g_startMode = 0;
int g_spawnAt = 0;              // 0 = at the crosshair, 1 = in front of me at a distance
bool g_lookAlways = false;
double g_lookT = 0;

// free camera pose for the GSC side (without it the script uses the player's own view)
std::string ViewArgs() {
    if (!g.freecam) return "";
    char b[96];
    std::snprintf(b, sizeof b, " v %.1f %.1f %.1f %.2f %.2f", g.cam.pose.pos.x, g.cam.pose.pos.y, g.cam.pose.pos.z, g.cam.pose.pitch, g.cam.pose.yaw);
    return b;
}
void SendView(const char* cmd, int id) {
    char b[160];
    if (id > 0) std::snprintf(b, sizeof b, "%s %d%s", cmd, id, ViewArgs().c_str());
    else std::snprintf(b, sizeof b, "%s%s", cmd, ViewArgs().c_str());
    ZmSend(b);
}
bool g_god = false;

double NowSec() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

void ExecStep(int i) {
    if (i < 0 || i >= (int)g_seq.size()) return;
    const char* tok = kActs[std::max(0, std::min(kActCount - 1, g_seq[i].act))].token;
    char b[96];
    if (g_target == 1) { std::snprintf(b, sizeof b, "all %s", tok); ZmSend(b); }
    else if (g_sel >= 0 && g_sel < (int)g_ids.size()) { std::snprintf(b, sizeof b, "act %d %s", g_ids[g_sel], tok); ZmSend(b); }
}

void RunSeq() {                  // runs the actions; if they already ran once, the zombies are put back first
    if (g_seq.empty()) return;
    if (g_dirty && !g_spawnLog.empty()) { ZmSend("reset"); g_dirty = false; g_run = false; g_headId = 0; g_runDelay = 1.8; }
    else { g_run = true; g_stepIdx = 0; g_stepT = 0; g_dirty = true; ExecStep(0); }
}

void Spawn() {
    for (int i = 0; i < g_count; ++i) {
        int id = g_nextId++;
        float side = (i - (g_count - 1) * 0.5f) * g_spacing;
        char b[260];
        std::string mt = (g_model[0] && std::string(g_model) != "default") ? std::string(g_model) : ModelToken();
        std::snprintf(b, sizeof b, "spawn %d %.0f %.0f %s %s%s", id, g_spawnAt == 0 ? -1.0f : g_dist, side, kStartModes[g_startMode], mt.c_str(), ViewArgs().c_str());
        ZmSend(b);
        g_spawnLog.push_back(b);
        g_ids.push_back(id);
    }
    g_dirty = false;
    g_sel = (int)g_ids.size() - 1;
}

}  // namespace


// ---- tracker: the game script keeps the dvars mvm_track / mvm_track2 up to date ("MVMT x*10 y*10 z*10 extra").
// The DLL finds those dvar structures in memory once (name pointer -> struct -> pointers to "MVMT" strings) and reads them each frame.
namespace {
std::vector<uint32_t> g_trkSlots[2];          // addresses of fields that point at the value string (current / latched / reset)
std::atomic<int> g_trkState{0};               // 0 idle, 1 searching, 2 ready, 3 not found
int g_trkId[2] = {-1, -1};

template <class F>
void ForEachMem(F fn) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    const size_t CH = 1 << 20;
    std::vector<uint8_t> buf(CH + 96);
    uint8_t* p = (uint8_t*)si.lpMinimumApplicationAddress;
    uint8_t* end = (uint8_t*)si.lpMaximumApplicationAddress;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
        uint8_t* base = (uint8_t*)mbi.BaseAddress;
        size_t sz = mbi.RegionSize;
        bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        if (ok) {
            for (size_t off = 0; off < sz; off += CH) {
                size_t n = std::min(CH + 64, sz - off);
                SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), base + off, buf.data(), n, &got) || got < 8) continue;
                fn(buf.data(), std::min<size_t>(got, CH), got, base + off);
            }
        }
        p = base + sz;
        if (p <= base) break;
    }
}

bool ReadStr(uint32_t addr, char* out, size_t n) {          // out is always zero terminated
    SIZE_T got = 0;
    std::memset(out, 0, n);
    if (addr < 0x10000) return false;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr, out, n - 1, &got) || got == 0) return false;
    return true;
}

void TrackScan() {
    g_trkState = 1;
    const uint32_t K = 0x3C6B91D5u;
    char nm[2][12] = {{'m','v','m','_','t','r','a','c','k',0,0,0}, {'m','v','m','_','t','r','a','c','k','2',0,0}};
    size_t nl[2] = {10, 11};                               // including the terminating zero
    struct Cand { uint32_t xa; int slot; };
    std::vector<Cand> cands;
    ForEachMem([&](uint8_t* d, size_t lim, size_t got, uint8_t* addr) {
        for (size_t i = 0; i + 12 < got && i < lim; ++i) {
            if (d[i] != 'm' || (i > 0 && d[i - 1] != 0)) continue;
            for (int s = 0; s < 2; ++s)
                if (std::memcmp(&d[i], nm[s], nl[s]) == 0) cands.push_back({(uint32_t)(uintptr_t)(addr + i) ^ K, s});
        }
    });
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.xa < b.xa; });
    std::vector<uint32_t> found[2];
    if (!cands.empty()) {
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (auto& c : cands) { uint32_t a = c.xa ^ K; lo = std::min(lo, a); hi = std::max(hi, a); }
        ForEachMem([&](uint8_t* d, size_t lim, size_t got, uint8_t* addr) {
            const uint32_t* w = (const uint32_t*)d;
            size_t cnt = std::min(lim, got) / 4;
            for (size_t i = 0; i < cnt; ++i) {
                uint32_t v = w[i];
                if (v < lo || v > hi) continue;
                uint32_t x = v ^ K;
                auto it = std::lower_bound(cands.begin(), cands.end(), x, [](const Cand& a, uint32_t b) { return a.xa < b; });
                if (it == cands.end() || it->xa != x) continue;
                int s = it->slot;
                uint32_t S = (uint32_t)(uintptr_t)(addr + i * 4);          // a dvar structure starts with its name pointer
                uint32_t f[24]; SIZE_T r = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)S, f, sizeof f, &r) || r < sizeof f) continue;
                for (int k = 1; k < 24; ++k) {
                    char t[8];
                    if (!ReadStr(f[k], t, 8)) continue;
                    if (std::memcmp(t, "MVMT", 4) == 0) found[s].push_back(S + 4 * k);
                }
            }
        });
    }
    g_trkSlots[0] = found[0]; g_trkSlots[1] = found[1];
    g_trkState = (!found[0].empty() && !found[1].empty()) ? 2 : 3;
    Log("tracker: slot0 %d fields, slot1 %d fields", (int)found[0].size(), (int)found[1].size());
}

}  // namespace

int TrackState() { return g_trkState; }
void TrackSet(int id, int slot) {
    slot = slot ? 1 : 0;
    g_trkId[slot] = id;
    char b[32]; std::snprintf(b, sizeof b, "%s %d", slot ? "track2" : "track", id);
    ZmSend(b);
    int st = 0;
    if (id != -1 && g_trkState.compare_exchange_strong(st, 1)) std::thread([] { TrackScan(); }).detach();
}
bool TrackGet(Vec3* p, int slot, float* extra) {
    slot = slot ? 1 : 0;
    if (g_trkState != 2) return false;
    for (uint32_t fld : g_trkSlots[slot]) {
        uint32_t sp = 0; SIZE_T r = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)fld, &sp, 4, &r) || r < 4) continue;
        char t[48];
        if (!ReadStr(sp, t, sizeof t)) continue;
        long x, y, zc, e;
        if (std::sscanf(t, "MVMT %ld %ld %ld %ld", &x, &y, &zc, &e) != 4) continue;
        if (x == 0 && y == 0 && zc == 0) continue;          // still the starting value
        p->x = x / 10.0f; p->y = y / 10.0f; p->z = zc / 10.0f;
        if (extra) *extra = e / 10.0f;
        return true;
    }
    return false;
}
int ZmSelectedId() { return (g_sel >= 0 && g_sel < (int)g_ids.size()) ? g_ids[g_sel] : 0; }

// ---- scene presets: the spawned zombies + the action steps, saved by name
static std::string SceneDir() { return g.dir + "scenes\\"; }
void ZmSceneSave(const std::string& name) {
    CreateDirectoryA(SceneDir().c_str(), nullptr);
    FILE* f = std::fopen((SceneDir() + name + ".bo2scene").c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "bo2scene 1\n");
    for (auto& s : g_spawnLog) std::fprintf(f, "%s\n", s.c_str());
    for (auto& st : g_seq) std::fprintf(f, "step %d %.3f\n", st.act, st.wait);
    std::fclose(f);
}
bool ZmSceneLoad(const std::string& name) {
    FILE* f = std::fopen((SceneDir() + name + ".bo2scene").c_str(), "rb");
    if (!f) return false;
    char line[512];
    std::vector<std::string> spawns; std::vector<Step> steps;
    bool hdr = false;
    while (std::fgets(line, sizeof line, f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (!hdr) { if (l.rfind("bo2scene", 0) == 0) hdr = true; else break; continue; }
        if (l.rfind("spawn ", 0) == 0) spawns.push_back(l);
        else { int a; float w; if (std::sscanf(l.c_str(), "step %d %f", &a, &w) == 2) { Step s; s.act = a; s.wait = w; steps.push_back(s); } }
    }
    std::fclose(f);
    if (!hdr) return false;
    ZmSend("clear");
    g_spawnLog.clear(); g_ids.clear(); g_run = false; g_headId = 0;
    for (auto& s : spawns) {
        size_t a = s.find(' ', 6);                              // "spawn <id> rest"
        if (a == std::string::npos) continue;
        int id = g_nextId++;
        char head[32]; std::snprintf(head, sizeof head, "spawn %d", id);
        std::string cmd = std::string(head) + s.substr(a);
        ZmSend(cmd);
        g_spawnLog.push_back(cmd);
        g_ids.push_back(id);
    }
    if (!steps.empty()) g_seq = steps;
    g_dirty = false;
    g_sel = g_ids.empty() ? -1 : (int)g_ids.size() - 1;
    return true;
}
std::vector<std::string> ZmSceneList() {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((SceneDir() + "*.bo2scene").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do { std::string n = fd.cFileName; if (n.size() > 9) out.push_back(n.substr(0, n.size() - 9)); } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

// ---- dolly sync: the overlay starts / stops the action sequence together with the camera path
bool ZmHasSeq() { return !g_seq.empty(); }
bool ZmSceneDirty() { return g_dirty && !g_spawnLog.empty(); }
void ZmSeqStart() { if (g_seq.empty()) return; g_run = true; g_stepIdx = 0; g_stepT = 0; g_dirty = true; ExecStep(0); }
void ZmSeqStop() { g_run = false; }
void ZmSceneReset() {            // new take: the game script removes everything (bodies too) and respawns each zombie where it started
    ZmSend("reset");
    g_dirty = false; g_run = false; g_headId = 0; g_runDelay = 0;
}
// when each step fires, counted from the start of the sequence (for the timeline)
int ZmSeqTimes(float* t, const char** label, int maxN) {
    float acc = 0; int n = 0;
    for (int i = 0; i < (int)g_seq.size() && n < maxN; ++i) {
        t[n] = acc; label[n] = kActs[std::max(0, std::min(kActCount - 1, g_seq[i].act))].label; ++n;
        acc += g_seq[i].wait;
    }
    return n;
}

void ZmQuickSpawn() {
    int sv = g_spawnAt, sc = g_count;
    g_spawnAt = 0; g_count = 1;
    Spawn();
    g_spawnAt = sv; g_count = sc;
}
void ZmQuickMove() { if (g_sel >= 0 && g_sel < (int)g_ids.size()) SendView("tp", g_ids[g_sel]); }
void ZmQuickFreeze() {
    static bool frozen = false;
    frozen = !frozen;
    ZmSend(frozen ? "all idle" : "all walk");
}

void ZmSend(const std::string& cmd) {
    if (!g_pending.empty()) g_pending += "|";
    g_pending += cmd;
}

// called once per rendered frame
void ZombiesUpdate(void* swapChain) {
    static double last = 0;
    static double firstSeen = 0; static bool probed = false;
    double now = NowSec();
    if (firstSeen == 0) firstSeen = now;
    if (!probed && now - firstSeen > 45.0) { probed = true; std::thread([] { ProbeEngine(); }).detach(); }
    static bool autoScanned = false;
    if (!autoScanned && now - firstSeen > 25.0 && g_scanState == 0) { autoScanned = true; std::thread([] { ScanModels(); }).detach(); }
    double dt = last > 0 ? now - last : 0;
    last = now;
    if (dt < 0 || dt > 0.25) dt = 0;
    if (g_lookAlways && !g_ids.empty() && now - g_lookT > 0.35) { g_lookT = now; SendView("lookall", 0); }

    // keep a device for the preview textures
    if (swapChain) {
        ID3D11Device* d = nullptr;
        ((IDXGISwapChain*)swapChain)->GetDevice(__uuidof(ID3D11Device), (void**)&d);
        if (d) { if (g_dev != d) { if (g_dev) g_dev->Release(); g_dev = d; g_thumbs.clear(); } else d->Release(); }
    }

    // preview pictures ("studio shot"): the camera is held level and the zombie is placed a fixed distance ahead of it,
    // so every picture is framed the same no matter where you were looking. Needs the menu open (it keeps the camera still).
    {
        double gs = g.frozen ? 0.0 : std::max(0.0f, g.timescale);
        static bool enteredFree = false; static float savedPitch = 0, savedFov = 65;
        if (!g_prevActive && !g_prevQ.empty()) {
            if (!g.freecam) { if (!enteredFree) { enteredFree = true; g.wantEnter = true; g.wantLeave = false; } }
            else if (!g.menuOpen) { /* wait for the menu, otherwise the mouse would move the camera */ }
            else {
                savedPitch = g.cam.pose.pitch; savedFov = g.fov;
                g.cam.pose.pitch = g.cam.target.pitch = 8.0f;
                g.fov = 60.0f;
                g_prevCur = g_prevQ.front(); g_prevQ.erase(g_prevQ.begin());
                char b[300]; std::snprintf(b, sizeof b, "spawn 9000 260 0 idle %s%s", g_prevCur.token.c_str(), ViewArgs().c_str());
                ZmSend(b);
                g_prevActive = true; g_prevT = 0;
            }
        }
        if (g_prevActive) {
            g_prevT += dt * gs;
            if (!g.freecam) {                                                      // left free camera mid-shot: drop it and put the zoom back
                ZmSend("act 9000 remove"); g_prevActive = false; g_prevQ.clear(); g.fov = savedFov; enteredFree = false;
                QueueCmd("cg_fov %d", g.cfg.restore_fov);
            }
            else if (!g.menuOpen) { g_prevT = 0; }                       // hold until the shot is possible again
            else if (g_prevT >= 3.0) {
                std::vector<uint8_t> px;
                if (swapChain && g_dev && GrabThumb(swapChain, kThumb, 0.62f, 0.17f, &px)) {
                    SaveThumb(g_prevCur.key, px);
                    ThumbTex& t = g_thumbs[g_prevCur.key];
                    if (t.srv) { t.srv->Release(); t.srv = nullptr; }
                    t.srv = MakeTex(px.data()); t.tried = true;
                }
                ZmSend("act 9000 remove");
                g_prevActive = false;
                g.cam.pose.pitch = g.cam.target.pitch = savedPitch; g.fov = savedFov;
                if (g_prevQ.empty() && enteredFree) { g.wantLeave = true; g.wantEnter = false; enteredFree = false; }
            }
        }
        if (!g_prevActive && g_prevQ.empty() && enteredFree && g.freecam) { g.wantLeave = true; enteredFree = false; }
    }

    if (g_runDelay > 0) {
        g_runDelay -= dt;
        if (g_runDelay <= 0) { g_runDelay = 0; g_run = true; g_stepIdx = 0; g_stepT = 0; g_dirty = true; ExecStep(0); }
    }

    // action sequence runs on game time, so slow motion / freeze stretch it like everything else
    if (g_run) {
        double gs = g.frozen ? 0.0 : std::max(0.0f, g.timescale);
        g_stepT += dt * gs;
        if (g_stepIdx < (int)g_seq.size() && g_stepT >= g_seq[g_stepIdx].wait) {
            g_stepT = 0;
            ++g_stepIdx;
            if (g_stepIdx >= (int)g_seq.size()) g_run = false;
            else ExecStep(g_stepIdx);
        }
    }

    // the game script polls the dvar; keep commands apart so none get overwritten
    if (!g_pending.empty()) {
        double ts = std::max(0.02f, g.frozen ? 1.0f : g.timescale);
        double gap = std::max(0.2, 0.08 / ts);
        if (now - g_lastSend >= gap) {
            std::string chunk;
            size_t pos = 0;
            while (pos < g_pending.size()) {
                size_t bar = g_pending.find('|', pos);
                std::string one = g_pending.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
                if (!chunk.empty() && chunk.size() + 1 + one.size() > 400) break;
                if (!chunk.empty()) chunk += "|";
                chunk += one;
                pos = bar == std::string::npos ? g_pending.size() : bar + 1;
            }
            g_pending.erase(0, pos);
            QueueCmd("set mvm_cmd \"%s\"", chunk.c_str());
            g_lastSend = now;
        }
    }
}

static bool Half(const char* l, int n = 2) {
    float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (n - 1)) / n;
    return ImGui::Button(l, ImVec2(w, 0));
}

void DrawZombiesTab() {
    ImGui::TextDisabled("needs mvm_zm.gsc in storage\\t6\\scripts\\zm - start a new match after adding it");
    bool hasSel = g_sel >= 0 && g_sel < (int)g_ids.size();
    UiCols2Begin("zcols");
    UiColNext();

    // ---------------------------------------------------------------- spawn
    UiCardBegin("spawn", "z_spawn");
    ImGui::RadioButton("at my crosshair", &g_spawnAt, 0); ImGui::SameLine();
    ImGui::RadioButton("in front of me", &g_spawnAt, 1);
    ImGui::SliderInt("how many", &g_count, 1, 10);
    if (g_spawnAt == 1) ImGui::SliderFloat("distance", &g_dist, 100, 1500, "%.0f");
    ImGui::SliderFloat("spacing", &g_spacing, 30, 300, "%.0f");
    ImGui::TextDisabled("start as");
    for (int i = 0; i < kMoveCount; ++i) {
        if (i && (i % 3)) ImGui::SameLine();
        if (ActCard(i, g_startMode == i, 76, 76, true)) g_startMode = i;
    }
    if (Half("spawn")) Spawn();
    ImGui::SameLine();
    if (Half("remove all")) { ZmSend("clear"); g_spawnLog.clear(); g_dirty = false; g_ids.clear(); g_sel = -1; g_run = false; g_headId = 0; }
    if (ImGui::Checkbox("invulnerable", &g_god)) ZmSend(g_god ? "god 1" : "god 0");
    UiCardEnd();

    // ---------------------------------------------------------------- place
    UiCardBegin("place and freeze", "z_place");
    ImGui::BeginDisabled(!hasSel);
    if (Half("to crosshair")) SendView("tp", g_ids[g_sel]);
    ImGui::SameLine();
    if (Half("look at me")) SendView("look", g_ids[g_sel]);
    if (Half("freeze")) { char b[48]; std::snprintf(b, sizeof b, "act %d idle", g_ids[g_sel]); ZmSend(b); }
    ImGui::SameLine();
    if (Half("unfreeze")) { char b[48]; std::snprintf(b, sizeof b, "act %d walk", g_ids[g_sel]); ZmSend(b); }
    ImGui::EndDisabled();
    ImGui::Separator();
    if (Half("all to crosshair")) SendView("tpall", 0);
    ImGui::SameLine();
    if (Half("all look at me")) SendView("lookall", 0);
    if (Half("freeze all")) ZmSend("all idle");
    ImGui::SameLine();
    if (Half("unfreeze all")) ZmSend("all walk");
    ImGui::Checkbox("always look at me", &g_lookAlways);
    UiCardEnd();

    // ---------------------------------------------------------------- list
    UiCardBegin("zombies", "z_list");
    if (g_ids.empty()) ImGui::TextDisabled("none spawned yet");
    for (int i = 0; i < (int)g_ids.size(); ++i) {
        char l[32]; std::snprintf(l, sizeof l, "zombie #%d", g_ids[i]);
        if (i && (i % 3)) ImGui::SameLine();
        if (ImGui::Selectable(l, g_sel == i, 0, ImVec2(86, 0))) g_sel = i;
    }
    ImGui::BeginDisabled(!hasSel);
    if (ImGui::Button(g_headId ? "leave head view" : "head view of selected", ImVec2(-1, 0))) {
        if (g_headId) { ZmSend("head 0"); g_headId = 0; }
        else { g_headId = g_ids[g_sel]; char b[32]; std::snprintf(b, sizeof b, "head %d", g_headId); ZmSend(b); }
    }
    ImGui::EndDisabled();
    UiCardEnd();

    UiColNext();
    // ---------------------------------------------------------------- model
    UiCardBegin("model", "z_model");
    ModelUI();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##cm", g_model, sizeof g_model);
    ImGui::TextDisabled("custom model name (advanced)");
    UiCardEnd();

    // ---------------------------------------------------------------- my character
    UiCardBegin("my character", "z_me");
    ImGui::TextDisabled("change how YOUR character looks (seen in free camera / third person)");
    PickerRow(1);
    if (Half("apply to me")) ZmSend("pmodel " + TokenOf(g_pBody, g_pHead, g_pExtra));
    ImGui::SameLine();
    if (Half("back to my normal one")) ZmSend("pdefault");
    static bool showSelf = false;
    if (ImGui::Checkbox("show my character in free camera (experimental)", &showSelf)) QueueCmd("cg_thirdPerson %d", showSelf ? 1 : 0);
    UiCardEnd();

    // ---------------------------------------------------------------- action editor
    UiCardBegin("action editor", "z_actions");
    ImGui::TextDisabled("do an action, wait, then do the next one");
    ImGui::RadioButton("selected zombie", &g_target, 0); ImGui::SameLine();
    ImGui::RadioButton("all zombies", &g_target, 1);

    int del = -1, up = -1;
    for (int i = 0; i < (int)g_seq.size(); ++i) {
        ImGui::PushID(i);
        bool cur = g_run && g_stepIdx == i;
        ImGui::TextColored(cur ? ImVec4(0.4f, 1, 0.4f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1), "%d", i + 1);
        ImGui::SameLine();
        ActPicker("act", &g_seq[i].act, kActCount);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::DragFloat("##w", &g_seq[i].wait, 0.05f, 0.0f, 600.0f, "%.2f s");
        ImGui::SameLine();
        if (ImGui::SmallButton("up")) up = i;
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) del = i;
        ImGui::PopID();
    }
    if (up > 0) std::swap(g_seq[up], g_seq[up - 1]);
    if (del >= 0) g_seq.erase(g_seq.begin() + del);

    if (Half("+ add step", 3)) g_seq.push_back(Step());
    ImGui::SameLine();
    if (Half("clear", 3)) { g_seq.clear(); g_run = false; }
    ImGui::SameLine();
    ImGui::BeginDisabled(g_seq.empty());
    if (!g_run && g_runDelay <= 0) { if (Half("run", 3)) RunSeq(); }
    else        { if (Half("stop", 3)) { g_run = false; g_runDelay = 0; } }
    ImGui::EndDisabled();

    ImGui::TextDisabled("quick");
    if (ImGui::SmallButton("walk -> headshot")) { g_seq = { {1, 3.0f}, {7, 1.0f} }; }
    ImGui::SameLine();
    if (ImGui::SmallButton("sprint -> explosion")) { g_seq = { {3, 2.0f}, {12, 1.0f} }; }
    if (ImGui::SmallButton("idle -> run -> melee kill")) { g_seq = { {0, 2.0f}, {2, 3.0f}, {11, 1.0f} }; }
    ImGui::TextDisabled("timers follow timescale");
    UiCardEnd();
    UiCols2End();
}

}  // namespace mvm
