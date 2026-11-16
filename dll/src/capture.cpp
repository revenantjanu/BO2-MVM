// capture.cpp - frame capture from the DXGI back buffer.
//   format 0: raw frames piped to ffmpeg (libx264, crf from settings)
//   format 1: numbered 32 bit TGA files
// Frames are taken in the Present hook *before* ImGui draws, so the menu is never in the video.
#include "mvm.h"
#include <d3d11.h>
#include <dxgi.h>
#include <vector>
#include <ctime>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <atomic>
#include <thread>

namespace mvm {
namespace {

struct Cap {
    bool active = false;
    HANDLE proc = nullptr;
    HANDLE pipe = nullptr;      // write end of ffmpeg stdin
    ID3D11Texture2D* staging = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool swapRB = false;        // backbuffer is RGBA instead of BGRA
    double acc = 0;
    LARGE_INTEGER last;
    LARGE_INTEGER freq;
    unsigned frameNo = 0;
    std::string outBase;        // video file or tga prefix
    std::vector<uint8_t> frame;
    bool hudHiddenByCapture = false;
    unsigned dupes = 0;
    LARGE_INTEGER lastPresent;
    double dtAvg = 0;
    float tsBefore = 1.0f;
    int sub = 0;                // sub-frames collected for the current video frame (motion blur)
    std::vector<uint32_t> accum;
    std::vector<uint8_t> vigMask; UINT vigW = 0, vigH = 0; float vigStrength = -1;
    uint32_t rng = 12345;
    unsigned dupes_unused = 0;         // frames repeated because the game rendered slower than the capture rate
    std::string lastFile;
} c;
std::atomic<int> g_ffInstall{0};   // 0 idle, 1 downloading, 2 done, 3 failed

bool IsBGRA(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM;
}
bool IsRGBA(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

std::string Stamp() {
    time_t t = time(nullptr);
    tm lt;
    localtime_s(&lt, &t);
    char b[64];
    std::strftime(b, sizeof b, "%Y%m%d_%H%M%S", &lt);
    return b;
}

bool g_tgaFallback = false;

// find ffmpeg: configured path, next to the dll, then PATH
std::string ResolveFfmpeg() {
    char buf[MAX_PATH];
    auto exists = [](const std::string& p) { DWORD a = GetFileAttributesA(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); };
    std::string cfg = g.cfg.ffmpeg_path;
    if (!cfg.empty() && exists(cfg)) return cfg;
    for (const char* rel : {"ffmpeg.exe", "ffmpeg\\bin\\ffmpeg.exe", "ffmpeg\\ffmpeg.exe"})
        if (exists(g.dir + rel)) return g.dir + rel;
    if (SearchPathA(nullptr, "ffmpeg.exe", nullptr, MAX_PATH, buf, nullptr)) return buf;
    return "";
}

bool StartFfmpeg(UINT w, UINT h, bool rgba) {
    std::string ff = ResolveFfmpeg();
    if (ff.empty()) return false;
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 22)) return false;
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);

    char cmd[1024];
    std::snprintf(cmd, sizeof cmd,
                  "\"%s\" -y -hide_banner -loglevel error -f rawvideo -pix_fmt %s -s %ux%u -r %d -i - -an "
                  "-c:v libx264 -preset veryfast -crf %d -pix_fmt yuv420p \"%s.mp4\"",
                  ff.c_str(), rgba ? "rgba" : "bgra", w, h, g.cfg.capture_fps, g.cfg.ffmpeg_crf, c.outBase.c_str());

    // ffmpeg's messages go to a log file so a failed encode can be explained
    SECURITY_ATTRIBUTES sl = {sizeof sl, nullptr, TRUE};
    HANDLE lg = CreateFileA((g.dir + "ffmpeg_log.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sl, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = rd;
    si.hStdOutput = lg != INVALID_HANDLE_VALUE ? lg : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = lg != INVALID_HANDLE_VALUE ? lg : GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, g.dir.c_str(), &si, &pi);
    CloseHandle(rd);
    if (lg != INVALID_HANDLE_VALUE) CloseHandle(lg);
    if (!ok) { CloseHandle(wr); return false; }
    c.lastFile = c.outBase + ".mp4";
    CloseHandle(pi.hThread);
    c.proc = pi.hProcess;
    c.pipe = wr;
    return true;
}

void WriteTga(const uint8_t* bgra, UINT w, UINT h, unsigned n) {
    char name[512];
    std::snprintf(name, sizeof name, "%s_%06u.tga", c.outBase.c_str(), n);
    FILE* f = std::fopen(name, "wb");
    if (!f) return;
    uint8_t hdr[18] = {0};
    hdr[2] = 2;
    hdr[12] = w & 255; hdr[13] = (w >> 8) & 255;
    hdr[14] = h & 255; hdr[15] = (h >> 8) & 255;
    hdr[16] = 32;
    hdr[17] = 0x28;   // 8 alpha bits, top-left origin
    std::fwrite(hdr, 1, 18, f);
    std::fwrite(bgra, 1, (size_t)w * h * 4, f);
    std::fclose(f);
}

void ReleaseStaging() {
    if (c.staging) { c.staging->Release(); c.staging = nullptr; }
}

}  // namespace

bool CaptureActive() { return c.active; }
unsigned CaptureFrames() { return c.frameNo; }
unsigned CaptureDupes() { return c.dupes; }
std::string CaptureLastFile() { return c.lastFile; }
bool FfmpegFound() { return !ResolveFfmpeg().empty(); }
int FfmpegInstallState() { return g_ffInstall.load(); }

// one-click video encoder: downloads a Windows ffmpeg build with PowerShell and puts ffmpeg.exe next to the dll
void FfmpegInstall() {
    if (g_ffInstall.load() == 1) return;
    g_ffInstall = 1;
    std::thread([] {
        std::string d = g.dir;
        // the install steps live in a script file, with a log next to it, and two download sources
        const char* ps =
            "$ErrorActionPreference = 'Continue'\r\n"
            "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12\r\n"
            "$ProgressPreference = 'SilentlyContinue'\r\n"
            "$d = $PSScriptRoot\r\n"
            "$z = Join-Path $d 'ffmpeg_dl.zip'\r\n"
            "$x = Join-Path $d 'ffmpeg_dl'\r\n"
            "$urls = @('https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip',\r\n"
            "          'https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip')\r\n"
            "foreach ($u in $urls) {\r\n"
            "  Write-Output \"trying $u\"\r\n"
            "  if (Test-Path $z) { Remove-Item $z -Force }\r\n"
            "  try { Invoke-WebRequest -Uri $u -OutFile $z -UseBasicParsing } catch { Write-Output \"web request failed: $_\" }\r\n"
            "  if (-not (Test-Path $z)) { try { & curl.exe -L --fail -o $z $u 2>&1 | Write-Output } catch { Write-Output \"curl failed: $_\" } }\r\n"
            "  if (Test-Path $z) {\r\n"
            "    if (Test-Path $x) { Remove-Item $x -Recurse -Force }\r\n"
            "    New-Item -ItemType Directory -Path $x -Force | Out-Null\r\n"
            "    try { Expand-Archive -Path $z -DestinationPath $x -Force } catch { Write-Output \"expand failed: $_\"; & tar.exe -xf $z -C $x 2>&1 | Write-Output }\r\n"
            "    $e = Get-ChildItem -Path $x -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue | Select-Object -First 1\r\n"
            "    if ($e) { Copy-Item $e.FullName (Join-Path $d 'ffmpeg.exe') -Force; Write-Output 'installed'; break }\r\n"
            "  }\r\n"
            "}\r\n"
            "if (Test-Path $z) { Remove-Item $z -Force -ErrorAction SilentlyContinue }\r\n"
            "if (Test-Path $x) { Remove-Item $x -Recurse -Force -ErrorAction SilentlyContinue }\r\n"
            "if (-not (Test-Path (Join-Path $d 'ffmpeg.exe'))) { exit 1 }\r\n";
        FILE* f = nullptr; fopen_s(&f, (d + "ffmpeg_install.ps1").c_str(), "wb");
        if (!f) { g_ffInstall = 3; return; }
        std::fwrite(ps, 1, std::strlen(ps), f); std::fclose(f);
        SECURITY_ATTRIBUTES sl = {sizeof sl, nullptr, TRUE};
        HANDLE lg = CreateFileA((d + "ffmpeg_install_log.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sl, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        std::string cmd = "powershell -NoProfile -ExecutionPolicy Bypass -File \"" + d + "ffmpeg_install.ps1\"";
        STARTUPINFOA si = {}; si.cb = sizeof si; si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES; si.wShowWindow = SW_HIDE;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = lg != INVALID_HANDLE_VALUE ? lg : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = si.hStdOutput;
        PROCESS_INFORMATION pi = {};
        std::vector<char> cl(cmd.begin(), cmd.end()); cl.push_back(0);
        BOOL ok = CreateProcessA(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, d.c_str(), &si, &pi);
        if (lg != INVALID_HANDLE_VALUE) CloseHandle(lg);
        if (!ok) { g_ffInstall = 3; return; }
        WaitForSingleObject(pi.hProcess, 600000);
        DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        DWORD a = GetFileAttributesA((d + "ffmpeg.exe").c_str());
        g_ffInstall = (code == 0 && a != INVALID_FILE_ATTRIBUTES) ? 2 : 3;
    }).detach();
}

// Grabs a square crop of the current back buffer (before the overlay is drawn), scaled to out x out RGBA.
// cropSide / cropTop are fractions of the screen height; the crop is centred horizontally.
bool GrabThumb(void* swapChainPtr, int out, float cropSide, float cropTop, std::vector<uint8_t>* rgba) {
    auto* sc = (IDXGISwapChain*)swapChainPtr;
    ID3D11Texture2D* back = nullptr;
    if (!sc || FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return false;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev);
    if (!dev) { back->Release(); return false; }
    dev->GetImmediateContext(&ctx);
    bool ok = false;
    D3D11_TEXTURE2D_DESC d;
    back->GetDesc(&d);
    ID3D11Texture2D* st = nullptr;
    if (d.SampleDesc.Count == 1 && (IsBGRA(d.Format) || IsRGBA(d.Format))) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st)) && st) {
            ctx->CopyResource(st, back);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                int side = std::max(8, (int)(d.Height * cropSide));
                side = std::min(side, (int)std::min(d.Width, d.Height));
                int x0 = ((int)d.Width - side) / 2;
                int y0 = std::max(0, std::min((int)d.Height - side, (int)(d.Height * cropTop)));
                rgba->assign((size_t)out * out * 4, 255);
                bool swap = IsBGRA(d.Format);
                for (int oy = 0; oy < out; ++oy)
                    for (int ox = 0; ox < out; ++ox) {
                        // box filter over the source pixels that map to this output pixel
                        int sx0 = x0 + ox * side / out, sx1 = std::max(sx0 + 1, x0 + (ox + 1) * side / out);
                        int sy0 = y0 + oy * side / out, sy1 = std::max(sy0 + 1, y0 + (oy + 1) * side / out);
                        unsigned acc[3] = {0, 0, 0}, n = 0;
                        for (int y = sy0; y < sy1; ++y) {
                            const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                            for (int x = sx0; x < sx1; ++x) { const uint8_t* px = row + (size_t)x * 4; acc[0] += px[0]; acc[1] += px[1]; acc[2] += px[2]; ++n; }
                        }
                        uint8_t* o = &(*rgba)[((size_t)oy * out + ox) * 4];
                        o[0] = (uint8_t)(acc[swap ? 2 : 0] / n); o[1] = (uint8_t)(acc[1] / n); o[2] = (uint8_t)(acc[swap ? 0 : 2] / n); o[3] = 255;
                    }
                ctx->Unmap(st, 0);
                ok = true;
            }
        }
    }
    if (st) st->Release();
    if (ctx) ctx->Release();
    dev->Release();
    back->Release();
    return ok;
}


// look: letterbox bars, vignette and film grain, baked into the frame (BGRA or RGBA, alpha untouched)
void PostFx(uint8_t* px, UINT w, UINT h) {
    const bool live = g_liveGrade && g_liveFxOk;
    if (!live && g.cfg.vignette > 0.01f) {
        if (c.vigW != w || c.vigH != h || std::fabs(c.vigStrength - g.cfg.vignette) > 0.001f) {
            c.vigW = w; c.vigH = h; c.vigStrength = g.cfg.vignette;
            c.vigMask.resize((size_t)w * h);
            float cx = w * 0.5f, cy = h * 0.5f, rmax = std::sqrt(cx * cx + cy * cy);
            for (UINT y = 0; y < h; ++y)
                for (UINT x = 0; x < w; ++x) {
                    float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / rmax;     // 0 centre .. 1 corner
                    float t = std::max(0.0f, std::min(1.0f, (d - 0.30f) / 0.70f));
                    t = t * t * (3 - 2 * t);
                    c.vigMask[(size_t)y * w + x] = (uint8_t)(255.0f * (1.0f - c.vigStrength * t));
                }
        }
        for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
            unsigned m = c.vigMask[i];
            uint8_t* p = px + i * 4;
            p[0] = (uint8_t)((p[0] * m) >> 8); p[1] = (uint8_t)((p[1] * m) >> 8); p[2] = (uint8_t)((p[2] * m) >> 8);
        }
    }
    if (!live && g.cfg.grain > 0.01f) {
        int amp = (int)(g.cfg.grain * 40.0f);
        for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
            c.rng ^= c.rng << 13; c.rng ^= c.rng >> 17; c.rng ^= c.rng << 5;
            int nz = (int)(c.rng & 255) - 128;
            nz = nz * amp / 128;
            uint8_t* p = px + i * 4;
            for (int k = 0; k < 3; ++k) { int v = p[k] + nz; p[k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
        }
    }
    static const float kRatio[] = {0, 2.39f, 2.0f, 1.85f};
    int lb = std::max(0, std::min(3, g.cfg.letterbox));
    if (lb) {
        UINT vis = (UINT)((float)w / kRatio[lb]);
        if (vis < h) {
            UINT bar = (h - vis) / 2;
            for (UINT y = 0; y < bar; ++y) { std::memset(px + (size_t)y * w * 4, 0, (size_t)w * 4); std::memset(px + (size_t)(h - 1 - y) * w * 4, 0, (size_t)w * 4); }
            // keep alpha opaque
            for (UINT y = 0; y < bar; ++y) for (UINT x = 0; x < w; ++x) { px[((size_t)y * w + x) * 4 + 3] = 255; px[((size_t)(h - 1 - y) * w + x) * 4 + 3] = 255; }
        }
    }
}


static inline uint8_t Cl8(float v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f); }

// colour grade + chromatic aberration + sharpen. rb = true when the buffer is RGBA (red is byte 0)
bool g_liveGrade = true;
void GradeFx(uint8_t* px, UINT w, UINT h, bool rb) {
    if (g_liveGrade && g_liveFxOk) return;       // the live pass already did all of this
    const int ri = rb ? 0 : 2, bi = rb ? 2 : 0;
    // chromatic aberration: shift red out and blue in, growing toward the edges
    if (g.cfg.chroma > 0.01f) {
        static std::vector<uint8_t> tmp;
        tmp.assign(px, px + (size_t)w * h * 4);
        float amt = g.cfg.chroma * 0.012f;                 // fraction of distance from centre
        float cx = w * 0.5f, cy = h * 0.5f;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                float dx = x - cx, dy = y - cy;
                int xr = (int)(x + dx * amt + (dx >= 0 ? 0.5f : -0.5f)), yr = (int)(y + dy * amt + (dy >= 0 ? 0.5f : -0.5f));
                int xb = (int)(x - dx * amt + (dx >= 0 ? 0.5f : -0.5f)), yb = (int)(y - dy * amt + (dy >= 0 ? 0.5f : -0.5f));
                xr = std::max(0, std::min((int)w - 1, xr)); yr = std::max(0, std::min((int)h - 1, yr));
                xb = std::max(0, std::min((int)w - 1, xb)); yb = std::max(0, std::min((int)h - 1, yb));
                uint8_t* o = px + ((size_t)y * w + x) * 4;
                o[ri] = tmp[((size_t)yr * w + xr) * 4 + ri];
                o[bi] = tmp[((size_t)yb * w + xb) * 4 + bi];
            }
    }
    // sharpen: unsharp mask against a 4-neighbour blur
    if (g.cfg.sharpen > 0.01f && w > 2 && h > 2) {
        static std::vector<uint8_t> src;
        src.assign(px, px + (size_t)w * h * 4);
        float k = g.cfg.sharpen * 0.9f;
        for (UINT y = 1; y + 1 < h; ++y)
            for (UINT x = 1; x + 1 < w; ++x) {
                size_t i = ((size_t)y * w + x) * 4;
                for (int ch = 0; ch < 3; ++ch) {
                    float n = (src[i - 4 + ch] + src[i + 4 + ch] + src[i - (size_t)w * 4 + ch] + src[i + (size_t)w * 4 + ch]) * 0.25f;
                    px[i + ch] = Cl8(src[i + ch] + (src[i + ch] - n) * k);
                }
            }
    }
    // colour grade through a 256-entry curve per channel (cheap: one table lookup per sample)
    float ex = g.cfg.exposure, co = g.cfg.contrast, sa = g.cfg.saturation, te = g.cfg.temperature, ti = g.cfg.tint;
    if (g_liveGrade) { ex = 0; co = 0; if (sa < 0) sa = 0; }     // those are already drawn by the game itself
    if (std::fabs(ex) < 0.005f && std::fabs(co) < 0.005f && std::fabs(sa) < 0.005f && std::fabs(te) < 0.005f && std::fabs(ti) < 0.005f) return;
    uint8_t lut[3][256];                                   // [0]=R [1]=G [2]=B
    float gain = std::pow(2.0f, ex);
    float cf = 1.0f + co * 0.9f;
    float rg = 1.0f + te * 0.12f - ti * 0.02f, bg = 1.0f - te * 0.12f - ti * 0.02f, gg = 1.0f - ti * 0.10f;
    float chg[3] = {rg, gg, bg};
    for (int ch = 0; ch < 3; ++ch)
        for (int v = 0; v < 256; ++v) {
            float f = v / 255.0f * gain * chg[ch];
            f = (f - 0.5f) * cf + 0.5f;
            lut[ch][v] = Cl8(f * 255.0f);
        }
    float satk = 1.0f + sa;
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        uint8_t* p = px + i * 4;
        float r = lut[0][p[ri]], gch = lut[1][p[1]], b = lut[2][p[bi]];
        if (satk != 1.0f) { float l = r * 0.299f + gch * 0.587f + b * 0.114f; r = l + (r - l) * satk; gch = l + (gch - l) * satk; b = l + (b - l) * satk; }
        p[ri] = Cl8(r); p[1] = Cl8(gch); p[bi] = Cl8(b);
    }
}


// bloom + anamorphic streaks: bright-pass on a quarter size copy, blurred, added back on top
void BloomFx(uint8_t* px, UINT w, UINT h, bool rb) {
    if (g.cfg.bloom < 0.01f && g.cfg.streak < 0.01f) return;
    const int ri = rb ? 0 : 2, bi = rb ? 2 : 0;
    UINT sw = w / 4, sh = h / 4;
    if (sw < 8 || sh < 8) return;
    static std::vector<float> a, b;
    a.assign((size_t)sw * sh * 3, 0.0f);
    for (UINT y = 0; y < sh; ++y)
        for (UINT x = 0; x < sw; ++x) {
            float r = 0, gg = 0, bl = 0;
            for (int dy = 0; dy < 4; ++dy)
                for (int dx = 0; dx < 4; ++dx) {
                    const uint8_t* p = px + (((size_t)(y * 4 + dy)) * w + (x * 4 + dx)) * 4;
                    r += p[ri]; gg += p[1]; bl += p[bi];
                }
            r /= 16.0f; gg /= 16.0f; bl /= 16.0f;
            float l = (r * 0.299f + gg * 0.587f + bl * 0.114f) / 255.0f;
            float k = std::max(0.0f, (l - 0.62f) / 0.38f);           // only the bright part blooms
            float* o = &a[((size_t)y * sw + x) * 3];
            o[0] = r * k; o[1] = gg * k; o[2] = bl * k;
        }
    auto blurH = [&](std::vector<float>& src, std::vector<float>& dst, int rad) {
        dst.assign(src.size(), 0.0f);
        for (UINT y = 0; y < sh; ++y) {
            for (int ch = 0; ch < 3; ++ch) {
                double acc = 0; int cnt = 0;
                for (int x = 0; x <= rad && x < (int)sw; ++x) { acc += src[((size_t)y * sw + x) * 3 + ch]; ++cnt; }
                for (int x = 0; x < (int)sw; ++x) {
                    dst[((size_t)y * sw + x) * 3 + ch] = (float)(acc / cnt);
                    int add = x + rad + 1, rem = x - rad;
                    if (add < (int)sw) { acc += src[((size_t)y * sw + add) * 3 + ch]; ++cnt; }
                    if (rem >= 0) { acc -= src[((size_t)y * sw + rem) * 3 + ch]; --cnt; }
                }
            }
        }
    };
    auto blurV = [&](std::vector<float>& src, std::vector<float>& dst, int rad) {
        dst.assign(src.size(), 0.0f);
        for (UINT x = 0; x < sw; ++x) {
            for (int ch = 0; ch < 3; ++ch) {
                double acc = 0; int cnt = 0;
                for (int y = 0; y <= rad && y < (int)sh; ++y) { acc += src[((size_t)y * sw + x) * 3 + ch]; ++cnt; }
                for (int y = 0; y < (int)sh; ++y) {
                    dst[((size_t)y * sw + x) * 3 + ch] = (float)(acc / cnt);
                    int add = y + rad + 1, rem = y - rad;
                    if (add < (int)sh) { acc += src[((size_t)add * sw + x) * 3 + ch]; ++cnt; }
                    if (rem >= 0) { acc -= src[((size_t)rem * sw + x) * 3 + ch]; --cnt; }
                }
            }
        }
    };
    static std::vector<float> glow, str;
    glow.clear(); str.clear();
    if (g.cfg.bloom > 0.01f) {
        std::vector<float> t1, t2;
        blurH(a, t1, 6); blurV(t1, t2, 6); blurH(t2, t1, 6); blurV(t1, glow, 6);
    }
    if (g.cfg.streak > 0.01f) {
        std::vector<float> t1;
        blurH(a, t1, (int)(sw / 5)); blurH(t1, str, (int)(sw / 5));
    }
    float gk = g.cfg.bloom * 1.1f, sk = g.cfg.streak * 2.2f;
    for (UINT y = 0; y < h; ++y) {
        float fy = std::min((float)(sh - 1), y / 4.0f);
        for (UINT x = 0; x < w; ++x) {
            float fx = std::min((float)(sw - 1), x / 4.0f);
            size_t si = ((size_t)(UINT)fy * sw + (UINT)fx) * 3;
            float r = 0, gg = 0, bl = 0;
            if (!glow.empty()) { r += glow[si] * gk; gg += glow[si + 1] * gk; bl += glow[si + 2] * gk; }
            if (!str.empty()) { r += str[si] * sk * 0.55f; gg += str[si + 1] * sk * 0.75f; bl += str[si + 2] * sk * 1.0f; }   // cool tint like an anamorphic lens
            uint8_t* p = px + ((size_t)y * w + x) * 4;
            p[ri] = Cl8(p[ri] + r); p[1] = Cl8(p[1] + gg); p[bi] = Cl8(p[bi] + bl);
        }
    }
}

static void WriteBmp(const std::string& file, const uint8_t* bgra, UINT w, UINT h) {
    FILE* f = std::fopen(file.c_str(), "wb");
    if (!f) return;
    uint32_t imgSize = w * h * 4, fileSize = 54 + imgSize;
    uint8_t hd[54] = {'B', 'M'};
    std::memcpy(hd + 2, &fileSize, 4);
    uint32_t off = 54, dib = 40; int32_t W = (int32_t)w, H = -(int32_t)h; uint16_t pl = 1, bpp = 32;
    std::memcpy(hd + 10, &off, 4); std::memcpy(hd + 14, &dib, 4); std::memcpy(hd + 18, &W, 4); std::memcpy(hd + 22, &H, 4);
    std::memcpy(hd + 26, &pl, 2); std::memcpy(hd + 28, &bpp, 2); std::memcpy(hd + 34, &imgSize, 4);
    std::fwrite(hd, 1, 54, f);
    std::fwrite(bgra, 1, imgSize, f);
    std::fclose(f);
}

static std::atomic<bool> g_stillReq{false};
void CaptureStill() { g_stillReq = true; }

static void DoStill(IDXGISwapChain* sc) {
    g_stillReq = false;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return;
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    back->GetDevice(&dev);
    if (!dev) { back->Release(); return; }
    dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC d; back->GetDesc(&d);
    if (d.SampleDesc.Count == 1 && (IsBGRA(d.Format) || IsRGBA(d.Format))) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        ID3D11Texture2D* st = nullptr;
        if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st)) && st) {
            ctx->CopyResource(st, back);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                std::vector<uint8_t> fr((size_t)d.Width * d.Height * 4);
                for (UINT y = 0; y < d.Height; ++y) std::memcpy(&fr[(size_t)y * d.Width * 4], (uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)d.Width * 4);
                ctx->Unmap(st, 0);
                bool rb = IsRGBA(d.Format);
                GradeFx(fr.data(), d.Width, d.Height, rb);
                BloomFx(fr.data(), d.Width, d.Height, rb);
                PostFx(fr.data(), d.Width, d.Height);
                if (rb) for (size_t p = 0; p < fr.size(); p += 4) std::swap(fr[p], fr[p + 2]);
                for (size_t p = 3; p < fr.size(); p += 4) fr[p] = 255;
                std::string dir = g.dir + "captures\\";
                CreateDirectoryA(dir.c_str(), nullptr);
                std::string file = dir + "bo2mvm_still_" + Stamp() + ".bmp";
                WriteBmp(file, fr.data(), d.Width, d.Height);
                c.lastFile = file;
                g.status = "still saved: " + file;
            }
            st->Release();
        }
    }
    if (ctx) ctx->Release();
    dev->Release();
    back->Release();
}

void CaptureStart() {
    if (c.active) return;
    std::string dir = g.dir + "captures\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    c.outBase = dir + "bo2mvm_" + Stamp();
    c.frameNo = 0;
    c.acc = 0;
    c.dupes = 0;
    c.lastFile.clear();
    c.w = c.h = 0;
    QueryPerformanceFrequency(&c.freq);
    QueryPerformanceCounter(&c.last);
    c.active = true;                       // ffmpeg is started on the first frame, once the size is known
    g.capturing = true;
    c.sub = 0; c.accum.clear(); c.dtAvg = 0; c.tsBefore = g.timescale; QueryPerformanceCounter(&c.lastPresent);
    if (g.cfg.smooth_capture) { SetTimescale(g.timescale); QueueCmd("com_maxfps %d", g.cfg.restore_maxfps); }   // fixed-step render: no frame cap, game time follows the video
    else QueueCmd("com_maxfps %d", g.cfg.capture_fps);
    if (g.cfg.capture_hide_hud && !g.freecam) { QueueCmd("cg_draw2D 0"); c.hudHiddenByCapture = true; }
}

void CaptureStop() {
    if (!c.active) return;
    c.active = false;
    g.capturing = false;
    if (c.pipe) { CloseHandle(c.pipe); c.pipe = nullptr; }
    if (c.proc) {
        WaitForSingleObject(c.proc, 60000);                       // let ffmpeg finish writing the file
        DWORD code = 0;
        if (GetExitCodeProcess(c.proc, &code) && code != 0) g.status = "video encoder reported an error - see ffmpeg_log.txt";
        CloseHandle(c.proc); c.proc = nullptr;
    }
    ReleaseStaging();
    QueueCmd("com_maxfps %d", g.cfg.restore_maxfps);
    if (g.cfg.smooth_capture && !g.frozen) SetTimescale(c.tsBefore);
    if (c.hudHiddenByCapture) { QueueCmd("cg_draw2D 1"); c.hudHiddenByCapture = false; }
}

void CaptureFramePresent(void* swapChainPtr) {
    if (!c.active) { if (g_stillReq) DoStill((IDXGISwapChain*)swapChainPtr); return; }
    auto* sc = (IDXGISwapChain*)swapChainPtr;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int copies;
    const bool smooth = g.cfg.smooth_capture != 0;
    const int K = smooth ? std::max(1, std::min(16, g.cfg.blur_samples)) : 1;
    if (smooth) {
        // fixed step: every presented frame is captured, and the game clock is steered so that each one advances
        // exactly 1/(fps*K) seconds of game time, whatever the machine manages to render
        double realDt = (double)(now.QuadPart - c.lastPresent.QuadPart) / (double)c.freq.QuadPart;
        c.lastPresent = now;
        double F = (double)std::max(1, g.cfg.capture_fps);
        if (realDt <= 0.0001 || realDt > 0.5) realDt = 1.0 / F;
        c.dtAvg = c.dtAvg <= 0 ? realDt : c.dtAvg * 0.7 + realDt * 0.3;
        double ts = (double)std::max(0.01f, c.tsBefore) * g.rampMul / (F * K) / c.dtAvg;     // keeps your own slow motion setting
        ts = std::max(0.02, std::min(6.0, ts));
        if (!g.frozen && std::fabs(ts / std::max(0.0001f, g.timescale) - 1.0) > 0.03) { SetTimescaleFast((float)ts); }
        copies = 1;
    } else {
        c.acc += (double)(now.QuadPart - c.last.QuadPart) / (double)c.freq.QuadPart;
        c.last = now;
        double step = 1.0 / (double)std::max(1, g.cfg.capture_fps);
        copies = (int)(c.acc / step);
        if (copies <= 0) return;
        c.acc -= copies * step;
        if (copies > 6) copies = 6;           // never write a huge burst after a stall
        if (copies > 1) c.dupes += copies - 1;
    }

    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) return;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev);
    if (!dev) { back->Release(); return; }
    dev->GetImmediateContext(&ctx);

    D3D11_TEXTURE2D_DESC d;
    back->GetDesc(&d);
    if (d.SampleDesc.Count > 1 || (!IsBGRA(d.Format) && !IsRGBA(d.Format))) {
        g.status = "capture: unsupported back buffer format";
        c.active = false; g.capturing = false;
        goto done;
    }
    if (!c.staging || d.Width != c.w || d.Height != c.h || d.Format != c.fmt) {
        if (c.pipe && (d.Width != c.w || d.Height != c.h) && c.w) {   // resolution changed mid-capture: stop cleanly
            g.status = "capture stopped: resolution changed";
            CaptureStop();
            goto done;
        }
        ReleaseStaging();
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &c.staging))) { c.active = false; g.capturing = false; goto done; }
        c.w = d.Width; c.h = d.Height; c.fmt = d.Format;
        c.swapRB = IsRGBA(d.Format);
        c.frame.resize((size_t)c.w * c.h * 4);
        g_tgaFallback = false;
        if (g.cfg.capture_format == 0 && !c.pipe) {
            if (!StartFfmpeg(c.w, c.h, c.swapRB)) {
                g_tgaFallback = true;
                c.lastFile = c.outBase + "_000000.tga ...";
                g.status = "capture: ffmpeg not found, saving TGA frames instead (Capture page: install the video encoder once)";
            }
        }
    }
    ctx->CopyResource(c.staging, back);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(c.staging, 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < c.h; ++y)
            std::memcpy(&c.frame[(size_t)y * c.w * 4], (uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)c.w * 4);
        ctx->Unmap(c.staging, 0);
        if (K > 1) {                                   // motion blur: average K sub-frames into one video frame
            if (c.accum.size() != c.frame.size()) { c.accum.assign(c.frame.size(), 0); c.sub = 0; }
            for (size_t p = 0; p < c.frame.size(); ++p) c.accum[p] += c.frame[p];
            if (++c.sub < K) goto done;
            for (size_t p = 0; p < c.frame.size(); ++p) c.frame[p] = (uint8_t)(c.accum[p] / K);
            std::fill(c.accum.begin(), c.accum.end(), 0u);
            c.sub = 0;
        }
        GradeFx(c.frame.data(), c.w, c.h, c.swapRB);
        BloomFx(c.frame.data(), c.w, c.h, c.swapRB);
        PostFx(c.frame.data(), c.w, c.h);
        for (int i = 0; i < copies; ++i) {
            if (g.cfg.capture_format == 0 && !g_tgaFallback) {
                DWORD wr = 0;
                if (!WriteFile(c.pipe, c.frame.data(), (DWORD)c.frame.size(), &wr, nullptr)) { CaptureStop(); break; }
            } else {
                if (c.swapRB)
                    for (size_t p = 0; p < c.frame.size(); p += 4) std::swap(c.frame[p], c.frame[p + 2]);
                WriteTga(c.frame.data(), c.w, c.h, c.frameNo);
                if (c.swapRB)
                    for (size_t p = 0; p < c.frame.size(); p += 4) std::swap(c.frame[p], c.frame[p + 2]);
            }
            ++c.frameNo;
        }
    }
done:
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    back->Release();
}

}  // namespace mvm
