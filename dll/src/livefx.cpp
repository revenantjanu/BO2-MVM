// livefx.cpp - live screen effects drawn by the tool itself (the game ignores its own tweak dvars on retail builds).
// One full screen pass over the back buffer, right before the overlay is drawn: colour grade, temperature / tint,
// vignette, film grain, chromatic aberration and sharpen. The recording grabs the frame after this pass.
#include "mvm.h"
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>

namespace mvm {

bool g_liveFxOk = false;
float g_lightBright = 0.0f;                       // map light: overall brightness in stops
float g_lightCol[3] = {1.0f, 1.0f, 1.0f};         // map light: colour the whole scene is lit with

namespace {
ID3D11Device* s_dev = nullptr;
ID3D11VertexShader* s_vs = nullptr;
ID3D11PixelShader* s_ps = nullptr;
ID3D11Buffer* s_cb = nullptr;
ID3D11SamplerState* s_smp = nullptr;
ID3D11BlendState* s_bs = nullptr;
ID3D11DepthStencilState* s_ds = nullptr;
ID3D11RasterizerState* s_rs = nullptr;
ID3D11Texture2D* s_tex = nullptr;
ID3D11ShaderResourceView* s_srv = nullptr;
UINT s_w = 0, s_h = 0;
DXGI_FORMAT s_fmt = DXGI_FORMAT_UNKNOWN;
bool s_failed = false;

const char* kHlsl = R"(
cbuffer C : register(b0) { float4 p0; float4 p1; float4 p2; float4 p3; };
Texture2D T : register(t0);
SamplerState S : register(s0);
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO VS(uint id : SV_VertexID) {
    VO o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
float4 PS(VO i) : SV_Target {
    float2 uv = i.uv;
    float2 d = uv - 0.5;
    float ca = p1.w * 0.012;
    float3 c;
    c.r = T.Sample(S, uv + d * ca).r;
    c.g = T.Sample(S, uv).g;
    c.b = T.Sample(S, uv - d * ca).b;
    if (p2.x > 0.01) {
        float2 px = 1.0 / p2.zw;
        float3 n = (T.Sample(S, uv + float2(px.x, 0)).rgb + T.Sample(S, uv - float2(px.x, 0)).rgb +
                    T.Sample(S, uv + float2(0, px.y)).rgb + T.Sample(S, uv - float2(0, px.y)).rgb) * 0.25;
        c += (c - n) * p2.x * 0.9;
    }
    float gain = exp2(p0.x + p3.w);
    float cf = 1.0 + p0.y * 0.9;
    float3 chg = float3(1.0 + p0.w * 0.12 - p1.x * 0.02, 1.0 - p1.x * 0.10, 1.0 - p0.w * 0.12 - p1.x * 0.02);
    c = c * gain * chg * p3.rgb;
    c = (c - 0.5) * cf + 0.5;
    float l = dot(c, float3(0.299, 0.587, 0.114));
    c = l + (c - l) * (1.0 + p0.z);
    float r = length(d) / 0.7071;
    float t = smoothstep(0.0, 1.0, saturate((r - 0.30) / 0.70));
    c *= 1.0 - p1.y * t;
    float n2 = frac(sin(dot(uv * p2.zw + p2.y, float2(12.9898, 78.233))) * 43758.5453);
    c += (n2 - 0.5) * p1.z * 0.31;
    return float4(saturate(c), 1.0);
}
)";

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

void ReleaseAll() {
    Rel(s_vs); Rel(s_ps); Rel(s_cb); Rel(s_smp); Rel(s_bs); Rel(s_ds); Rel(s_rs); Rel(s_srv); Rel(s_tex);
    s_w = s_h = 0; s_fmt = DXGI_FORMAT_UNKNOWN;
}

bool Compile(const char* entry, const char* target, ID3DBlob** out) {
    typedef HRESULT(WINAPI * PFN)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    static PFN fn = nullptr;
    if (!fn) {
        const char* dlls[] = {"d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll"};
        for (const char* d : dlls) { HMODULE m = LoadLibraryA(d); if (m) { fn = (PFN)GetProcAddress(m, "D3DCompile"); if (fn) break; } }
    }
    if (!fn) return false;
    ID3DBlob* err = nullptr;
    HRESULT hr = fn(kHlsl, std::strlen(kHlsl), "livefx", nullptr, nullptr, entry, target, 0, 0, out, &err);
    if (FAILED(hr)) { if (err) { Log("livefx shader error: %s", (const char*)err->GetBufferPointer()); err->Release(); } return false; }
    if (err) err->Release();
    return true;
}

bool Init(ID3D11Device* dev) {
    ReleaseAll();
    ID3DBlob *vb = nullptr, *pb = nullptr;
    if (!Compile("VS", "vs_4_0", &vb)) return false;
    if (!Compile("PS", "ps_4_0", &pb)) { vb->Release(); return false; }
    bool ok = SUCCEEDED(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &s_vs)) &&
              SUCCEEDED(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &s_ps));
    vb->Release(); pb->Release();
    if (!ok) return false;
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &s_cb))) return false;
    D3D11_SAMPLER_DESC sd = {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &s_smp))) return false;
    D3D11_BLEND_DESC bl = {}; bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bl, &s_bs))) return false;
    D3D11_DEPTH_STENCIL_DESC dd = {};
    if (FAILED(dev->CreateDepthStencilState(&dd, &s_ds))) return false;
    D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, &s_rs))) return false;
    s_dev = dev;
    return true;
}
}  // namespace

static bool Neutral() {
    return std::fabs(g.cfg.exposure) < 0.005f && std::fabs(g.cfg.contrast) < 0.005f && std::fabs(g.cfg.saturation) < 0.005f &&
           std::fabs(g.cfg.temperature) < 0.005f && std::fabs(g.cfg.tint) < 0.005f && g.cfg.vignette < 0.01f &&
           g.cfg.grain < 0.01f && g.cfg.chroma < 0.01f && g.cfg.sharpen < 0.01f &&
           std::fabs(g_lightBright) < 0.005f && std::fabs(g_lightCol[0] - 1) < 0.005f && std::fabs(g_lightCol[1] - 1) < 0.005f && std::fabs(g_lightCol[2] - 1) < 0.005f;
}

void LiveFxPresent(void* swapChain) {
    if (!g_liveGrade || s_failed || !swapChain) { g_liveFxOk = false; return; }
    if (Neutral()) { g_liveFxOk = true; return; }          // nothing to draw, and nothing for the video to bake either
    IDXGISwapChain* sc = (IDXGISwapChain*)swapChain;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) { g_liveFxOk = false; return; }
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    bb->GetDevice(&dev);
    if (!dev) { bb->Release(); g_liveFxOk = false; return; }
    dev->GetImmediateContext(&ctx);
    bool ok = false;
    do {
        if (d.SampleDesc.Count != 1) break;
        if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_R10G10B10A2_UNORM) break;
        if (s_dev != dev || !s_vs) {
            if (!Init(dev)) { s_failed = true; Log("livefx: init failed, effects stay baked into the video only"); break; }
            Log("livefx: ready (%ux%u fmt %d)", d.Width, d.Height, (int)d.Format);
        }
        if (!s_tex || s_w != d.Width || s_h != d.Height || s_fmt != d.Format) {
            Rel(s_srv); Rel(s_tex);
            D3D11_TEXTURE2D_DESC td = d; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = 0; td.MiscFlags = 0; td.MipLevels = 1; td.ArraySize = 1;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &s_tex)) || FAILED(dev->CreateShaderResourceView(s_tex, nullptr, &s_srv))) { Rel(s_srv); Rel(s_tex); s_failed = true; break; }
            s_w = d.Width; s_h = d.Height; s_fmt = d.Format;
        }
        ID3D11RenderTargetView* rtv = nullptr;
        if (FAILED(dev->CreateRenderTargetView(bb, nullptr, &rtv)) || !rtv) break;
        ctx->CopyResource(s_tex, bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float v[16] = {g.cfg.exposure, g.cfg.contrast, g.cfg.saturation, g.cfg.temperature,
                           g.cfg.tint, g.cfg.vignette, g.cfg.grain, g.cfg.chroma,
                           g.cfg.sharpen, (float)(GetTickCount() % 100000) * 0.001f, (float)d.Width, (float)d.Height,
                           g_lightCol[0], g_lightCol[1], g_lightCol[2], g_lightBright};
            std::memcpy(m.pData, v, sizeof v);
            ctx->Unmap(s_cb, 0);
        }
        // The game caches its own pipeline state and does not rebind everything every frame, so whatever this pass
        // changes must be put back exactly, or the next frame's world draws with our state (black world, HUD fine).
        ID3D11RenderTargetView* oRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* oDsv = nullptr;
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oRtv, &oDsv);
        UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; D3D11_VIEWPORT oVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        ctx->RSGetViewports(&nvp, oVp);
        UINT nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; D3D11_RECT oSc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        ctx->RSGetScissorRects(&nsc, oSc);
        ID3D11RasterizerState* oRs = nullptr; ctx->RSGetState(&oRs);
        ID3D11BlendState* oBs = nullptr; FLOAT oBf[4] = {}; UINT oSm = 0; ctx->OMGetBlendState(&oBs, oBf, &oSm);
        ID3D11DepthStencilState* oDs = nullptr; UINT oSr = 0; ctx->OMGetDepthStencilState(&oDs, &oSr);
        ID3D11InputLayout* oIl = nullptr; ctx->IAGetInputLayout(&oIl);
        D3D11_PRIMITIVE_TOPOLOGY oTop; ctx->IAGetPrimitiveTopology(&oTop);
        ID3D11VertexShader* oVs = nullptr; ctx->VSGetShader(&oVs, nullptr, nullptr);
        ID3D11PixelShader* oPs = nullptr; ctx->PSGetShader(&oPs, nullptr, nullptr);
        ID3D11GeometryShader* oGs = nullptr; ctx->GSGetShader(&oGs, nullptr, nullptr);
        ID3D11HullShader* oHs = nullptr; ctx->HSGetShader(&oHs, nullptr, nullptr);
        ID3D11DomainShader* oDom = nullptr; ctx->DSGetShader(&oDom, nullptr, nullptr);
        ID3D11Buffer* oCb = nullptr; ctx->PSGetConstantBuffers(0, 1, &oCb);
        ID3D11SamplerState* oSmp = nullptr; ctx->PSGetSamplers(0, 1, &oSmp);
        ID3D11ShaderResourceView* oSrv = nullptr; ctx->PSGetShaderResources(0, 1, &oSrv);
        D3D11_VIEWPORT vp = {0, 0, (float)d.Width, (float)d.Height, 0, 1};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(s_rs);
        ctx->OMSetBlendState(s_bs, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(s_ds, 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(s_vs, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->PSSetShader(s_ps, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &s_cb);
        ctx->PSSetSamplers(0, 1, &s_smp);
        ctx->PSSetShaderResources(0, 1, &s_srv);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nul = nullptr;
        ctx->PSSetShaderResources(0, 1, &nul);
        // put everything back
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oRtv, oDsv);
        ctx->RSSetViewports(nvp, oVp);
        ctx->RSSetScissorRects(nsc, oSc);
        ctx->RSSetState(oRs);
        ctx->OMSetBlendState(oBs, oBf, oSm);
        ctx->OMSetDepthStencilState(oDs, oSr);
        ctx->IASetInputLayout(oIl);
        ctx->IASetPrimitiveTopology(oTop);
        ctx->VSSetShader(oVs, nullptr, 0);
        ctx->PSSetShader(oPs, nullptr, 0);
        ctx->GSSetShader(oGs, nullptr, 0);
        ctx->HSSetShader(oHs, nullptr, 0);
        ctx->DSSetShader(oDom, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &oCb);
        ctx->PSSetSamplers(0, 1, &oSmp);
        ctx->PSSetShaderResources(0, 1, &oSrv);
        for (auto*& r : oRtv) if (r) { r->Release(); r = nullptr; }
        if (oDsv) oDsv->Release();
        if (oRs) oRs->Release(); if (oBs) oBs->Release(); if (oDs) oDs->Release(); if (oIl) oIl->Release();
        if (oVs) oVs->Release(); if (oPs) oPs->Release(); if (oGs) oGs->Release(); if (oHs) oHs->Release(); if (oDom) oDom->Release();
        if (oCb) oCb->Release(); if (oSmp) oSmp->Release(); if (oSrv) oSrv->Release();
        rtv->Release();
        ok = true;
    } while (0);
    g_liveFxOk = ok;
    if (ctx) ctx->Release();
    dev->Release();
    bb->Release();
}

}  // namespace mvm
