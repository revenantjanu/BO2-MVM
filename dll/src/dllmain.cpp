// dllmain.cpp - entry point. Everything real happens on a worker thread (never inside DllMain).
#include "mvm.h"
#include <cstring>
#include <cstdio>

namespace {

HMODULE g_self = nullptr;

// ---- crash diagnostics: the game leaves no crash report, so we write our own into bo2mvm_log.txt / crash.dmp
void DescribeAddr(void* a, char* out, size_t n) {
    HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m);
    char name[MAX_PATH] = "?";
    if (m) GetModuleFileNameA(m, name, MAX_PATH);
    const char* base = std::strrchr(name, '\\');
    std::snprintf(out, n, "%s+0x%X", base ? base + 1 : name, (unsigned)((uintptr_t)a - (uintptr_t)m));
}

void LogException(const char* tag, PEXCEPTION_POINTERS e) {
    char where[300];
    DescribeAddr(e->ExceptionRecord->ExceptionAddress, where, sizeof where);
    mvm::Log("%s code=%08lX at %s (addr %p) info=%p %p thread=%lu", tag, e->ExceptionRecord->ExceptionCode, where,
             e->ExceptionRecord->ExceptionAddress, (void*)e->ExceptionRecord->ExceptionInformation[0], (void*)e->ExceptionRecord->ExceptionInformation[1], GetCurrentThreadId());
    void* frames[12] = {};
    USHORT n = CaptureStackBackTrace(0, 12, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) { char w[300]; DescribeAddr(frames[i], w, sizeof w); mvm::Log("    #%u %s", (unsigned)i, w); }
}

LONG CALLBACK VehHandler(PEXCEPTION_POINTERS e) {
    static volatile LONG count = 0;
    DWORD c = e->ExceptionRecord->ExceptionCode;
    if ((c == 0xC0000005 || c == 0xC00000FD || c == 0xC000001D || c == 0xC0000096 || c == 0xC0000094 || c == 0xC000008E) && InterlockedIncrement(&count) <= 25)
        LogException("exception(first chance)", e);
    return EXCEPTION_CONTINUE_SEARCH;
}

#pragma pack(push, 4)
struct MdExc { DWORD ThreadId; PEXCEPTION_POINTERS ExceptionPointers; BOOL ClientPointers; };
#pragma pack(pop)

LONG WINAPI TopFilter(PEXCEPTION_POINTERS e) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    LogException("UNHANDLED EXCEPTION (crash)", e);
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    if (dbg) {
        typedef BOOL(WINAPI * MDWD)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
        MDWD w = (MDWD)GetProcAddress(dbg, "MiniDumpWriteDump");
        HANDLE f = CreateFileA((mvm::g.dir + "crash.dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (w && f != INVALID_HANDLE_VALUE) {
            MdExc me = {GetCurrentThreadId(), e, FALSE};
            w(GetCurrentProcess(), GetCurrentProcessId(), f, 0 /*MiniDumpNormal*/, &me, nullptr, nullptr);
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    mvm::Log("crash.dmp written");
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI InitThread(LPVOID) {
    using namespace mvm;
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, MAX_PATH);
    std::string p = path;
    g.dir = p.substr(0, p.find_last_of("\\/") + 1);
    { FILE* f=nullptr; fopen_s(&f,(g.dir+"bo2mvm_log.txt").c_str(),"w"); if(f) fclose(f); }
    Log("dll attached, dir=%s, cmdline=%s", g.dir.c_str(), GetCommandLineA());
    AddVectoredExceptionHandler(1, VehHandler);
    SetUnhandledExceptionFilter(TopFilter);
    g.cfg.Load(g.dir + "settings.ini");
    g.fov = g.cfg.default_fov;

    // wait until the game has a window and a loaded renderer
    for (int i = 0; i < 600 && !GetModuleHandleA("d3d11.dll"); ++i) Sleep(100);
    Sleep(500);

    Log("d3d11 loaded: %d", GetModuleHandleA("d3d11.dll") != nullptr);
    bool core = CoreInit(g_self);
    Log("CoreInit=%d status=%s", (int)core, g.status.c_str());     // refuses to run unless the game was started with -lan
    if (core) {
        if (!OverlayInit()) {
            core = false;
            g.status = "overlay failed: " + g.status;
        }
    }
    if (core) Log("OverlayInit ok; waiting for first Present");
    if (!core) {
        Log("FAILED: %s", g.status.c_str());
        std::string msg = "BO2 MVM could not start:\n" + g.status;
        MessageBoxA(nullptr, msg.c_str(), "BO2 MVM", MB_OK | MB_ICONWARNING | MB_TOPMOST);
    }
    if (core) MessageBoxA(nullptr, "BO2 MVM loaded.\nPress INSERT in game for the menu.", "BO2 MVM", MB_OK | MB_TOPMOST);
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        CloseHandle(CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr));
    }
    return TRUE;
}
