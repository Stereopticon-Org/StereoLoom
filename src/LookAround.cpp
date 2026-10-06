// LookAround.cpp -- see LookAround.h and shaders/Look.hlsl.
#include "LookAround.h"
#include <algorithm>
#include <cmath>

// The shaders, compiled at build time (shaders/Look.hlsl; see CMakeLists.txt).
#include "Look_VSFull.h"
#include "Look_VSSplat.h"
#include "Look_PSDownFirst.h"
#include "Look_PSDownNext.h"
#include "Look_PSGrad.h"
#include "Look_PSMatchFull.h"
#include "Look_PSMatchRefine.h"
#include "Look_PSCheck.h"
#include "Look_PSFill.h"
#include "Look_PSMedian.h"
#include "Look_PSSpread.h"
#include "Look_PSSplat.h"
#include "Look_PSHole.h"
#include "Look_PSCompose.h"
#include "Look_PSUnseen.h"

using namespace srw;

bool LookAround::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!device || !context) return false;
    m_device = device; m_context = context;
    HRESULT hr = device->CreateVertexShader(g_Look_VSFull, sizeof(g_Look_VSFull), nullptr, &m_vsFull);
    if (SUCCEEDED(hr)) hr = device->CreateVertexShader(g_Look_VSSplat, sizeof(g_Look_VSSplat), nullptr, &m_vsSplat);
    struct { const void* code; size_t size; ID3D11PixelShader** out; } ps[] = {
        { g_Look_PSDownFirst,   sizeof(g_Look_PSDownFirst),   &m_psDownFirst },
        { g_Look_PSDownNext,    sizeof(g_Look_PSDownNext),    &m_psDownNext },
        { g_Look_PSGrad,        sizeof(g_Look_PSGrad),        &m_psGrad },
        { g_Look_PSMatchFull,   sizeof(g_Look_PSMatchFull),   &m_psMatchFull },
        { g_Look_PSMatchRefine, sizeof(g_Look_PSMatchRefine), &m_psMatchRefine },
        { g_Look_PSCheck,       sizeof(g_Look_PSCheck),       &m_psCheck },
        { g_Look_PSFill,        sizeof(g_Look_PSFill),        &m_psFill },
        { g_Look_PSMedian,      sizeof(g_Look_PSMedian),      &m_psMedian },
        { g_Look_PSSpread,      sizeof(g_Look_PSSpread),      &m_psSpread },
        { g_Look_PSSplat,       sizeof(g_Look_PSSplat),       &m_psSplat },
        { g_Look_PSHole,        sizeof(g_Look_PSHole),        &m_psHole },
        { g_Look_PSCompose,     sizeof(g_Look_PSCompose),     &m_psCompose },
        { g_Look_PSUnseen,      sizeof(g_Look_PSUnseen),      &m_psUnseen },
    };
    for (const auto& p : ps)
        if (SUCCEEDED(hr)) hr = device->CreatePixelShader(p.code, p.size, nullptr, p.out);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CB); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(hr)) hr = device->CreateBuffer(&bd, nullptr, &m_cb);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(hr)) hr = device->CreateSamplerState(&sd, &m_sampler);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS;
    if (SUCCEEDED(hr)) hr = device->CreateDepthStencilState(&dd, &m_depthOn);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    if (SUCCEEDED(hr)) hr = device->CreateRasterizerState(&rd, &m_raster);
    if (FAILED(hr)) { Log("LookAround: initialise failed (0x%08X)", (unsigned)hr); Shutdown(); return false; }
    return true;
}

void LookAround::Release(Tex& t) { SAFE_RELEASE(t.srv); SAFE_RELEASE(t.rtv); SAFE_RELEASE(t.tex); t.w = t.h = 0; }

void LookAround::Shutdown()
{
    for (int k = 0; k < kLevels; ++k) { Release(m_tmp[k]); Release(m_lvl[k]); Release(m_dispA[k]); Release(m_dispB[k]); }
    Release(m_target); Release(m_targetFilled); Release(m_out[0]); Release(m_out[1]);
    SAFE_RELEASE(m_dsv); SAFE_RELEASE(m_depthTex);
    SAFE_RELEASE(m_disjoint); for (auto& q : m_ts) SAFE_RELEASE(q);
    SAFE_RELEASE(m_raster); SAFE_RELEASE(m_depthOn); SAFE_RELEASE(m_sampler); SAFE_RELEASE(m_cb);
    SAFE_RELEASE(m_psUnseen); SAFE_RELEASE(m_psCompose); SAFE_RELEASE(m_psHole); SAFE_RELEASE(m_psSplat);
    SAFE_RELEASE(m_psSpread);
    SAFE_RELEASE(m_psMedian); SAFE_RELEASE(m_psFill); SAFE_RELEASE(m_psCheck); SAFE_RELEASE(m_psMatchRefine);
    SAFE_RELEASE(m_psMatchFull); SAFE_RELEASE(m_psGrad); SAFE_RELEASE(m_psDownNext); SAFE_RELEASE(m_psDownFirst);
    SAFE_RELEASE(m_vsSplat); SAFE_RELEASE(m_vsFull);
    m_device = nullptr; m_context = nullptr; m_eyeW = m_eyeH = 0;
}

// A render target of that size (both eyes: twice w across), made again when the
// size changes. view: the format it is drawn to and read through, where that is
// not the texture's own (an _SRGB view of a typeless 8-bit texture).
bool LookAround::Ensure(Tex& t, int w, int h, DXGI_FORMAT fmt, DXGI_FORMAT view)
{
    if (t.tex && t.w == w && t.h == h) return true;
    Release(t);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &t.tex))) return false;
    const DXGI_FORMAT vf = view != DXGI_FORMAT_UNKNOWN ? view : fmt;
    D3D11_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = vf; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = vf; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
    if (FAILED(m_device->CreateRenderTargetView(t.tex, &rv, &t.rtv)) || FAILED(m_device->CreateShaderResourceView(t.tex, &sv, &t.srv))) { Release(t); return false; }
    t.w = w; t.h = h;
    return true;
}

// One full-target pass.
void LookAround::Pass(ID3D11PixelShader* ps, const Tex& target, const CB& cb, ID3D11ShaderResourceView* t0,
                      ID3D11ShaderResourceView* t1, ID3D11ShaderResourceView* t2)
{
    m_context->UpdateSubresource(m_cb, 0, nullptr, &cb, 0, 0);
    D3D11_VIEWPORT vp{}; vp.Width = (FLOAT)target.w; vp.Height = (FLOAT)target.h; vp.MaxDepth = 1.0f;
    m_context->OMSetRenderTargets(1, &target.rtv, nullptr);
    m_context->RSSetViewports(1, &vp);
    m_context->VSSetShader(m_vsFull, nullptr, 0);
    m_context->PSSetShader(ps, nullptr, 0);
    ID3D11ShaderResourceView* srvs[3] = { t0, t1, t2 };
    m_context->PSSetShaderResources(0, 3, srvs);
    m_context->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[3] = {};
    m_context->PSSetShaderResources(0, 3, nulls);
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
}

bool LookAround::Render(ID3D11ShaderResourceView* src, int eyeW, int eyeH, const View& view, bool newPicture)
{
    if (!m_context || !src || eyeW < 64 || eyeH < 64) return false;
    // The sizes: the finest about 480 texels across -- 1/8 of a 4K eye, 1/4 of a
    // 2K one (the eyes of a side-by-side game on a 4K screen: at 1/8 a head was
    // five texels wide, and came out wrong) -- then half that, and half again.
    const int base = eyeW >= 2600 ? 8 : eyeW >= 1300 ? 4 : 2;
    int w[kLevels], h[kLevels];
    w[0] = eyeW / base; h[0] = eyeH / base;
    for (int k = 1; k < kLevels; ++k) { w[k] = (std::max)(1, w[k - 1] / 2); h[k] = (std::max)(1, h[k - 1] / 2); }
    bool ok = true;
    for (int k = 0; k < kLevels && ok; ++k)
        ok = Ensure(m_tmp[k], 2 * w[k], h[k], DXGI_FORMAT_R16G16B16A16_FLOAT) && Ensure(m_lvl[k], 2 * w[k], h[k], DXGI_FORMAT_R16G16B16A16_FLOAT) &&
             Ensure(m_dispA[k], 2 * w[k], h[k], DXGI_FORMAT_R16G16B16A16_FLOAT) && Ensure(m_dispB[k], 2 * w[k], h[k], DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && Ensure(m_target, 2 * w[0], h[0], DXGI_FORMAT_R16G16_FLOAT) && Ensure(m_targetFilled, 2 * w[0], h[0], DXGI_FORMAT_R16G16_FLOAT) &&
         Ensure(m_out[0], 2 * eyeW, eyeH, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) &&
         Ensure(m_out[1], 2 * eyeW, eyeH, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    if (ok && (!m_dsv || m_eyeW != eyeW || m_eyeH != eyeH))
    {
        SAFE_RELEASE(m_dsv); SAFE_RELEASE(m_depthTex);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)(2 * w[0]); td.Height = (UINT)h[0]; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_D32_FLOAT;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ok = SUCCEEDED(m_device->CreateTexture2D(&td, nullptr, &m_depthTex)) && SUCCEEDED(m_device->CreateDepthStencilView(m_depthTex, nullptr, &m_dsv));
    }
    if (!ok) return false;
    if (m_eyeW != eyeW || m_eyeH != eyeH) m_haveMaps = false;
    m_eyeW = eyeW; m_eyeH = eyeH;

    // (Timed one frame in sixteen: five timestamps, read back when they are in.)
    if (m_timePending && m_disjoint)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (m_context->GetData(m_disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
        {
            UINT64 v[5] = {}; bool got = !dj.Disjoint && dj.Frequency > 0;
            for (int k = 0; k < 5 && got; ++k) got = m_context->GetData(m_ts[k], &v[k], sizeof(v[k]), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            if (got) { for (int k = 0; k < 4; ++k) m_ms[k] = (double)(v[k + 1] - v[k]) * 1000.0 / (double)dj.Frequency; m_haveTimes = true; }
            m_timePending = false;
        }
    }
    m_timing = !m_timePending && (m_frame++ % 16) == 0;
    auto mark = [&](int k) {
        if (!m_timing) return;
        if (!m_ts[k]) { D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP, 0 }; m_device->CreateQuery(&qd, &m_ts[k]); }
        if (m_ts[k]) m_context->End(m_ts[k]);
    };
    if (m_timing)
    {
        if (!m_disjoint) { D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }; m_device->CreateQuery(&qd, &m_disjoint); }
        if (m_disjoint) m_context->Begin(m_disjoint); else m_timing = false;
    }
    mark(0);

    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(nullptr);
    m_context->RSSetState(m_raster);
    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_context->OMSetDepthStencilState(nullptr, 0);
    m_context->PSSetSamplers(0, 1, &m_sampler);
    m_context->PSSetConstantBuffers(0, 1, &m_cb);
    m_context->VSSetConstantBuffers(0, 1, &m_cb);

    CB cb{};
    cb.tL = view.tL; cb.tR = view.tR; cb.winR = 3.0f;
    cb.ty = view.ty; cb.rho = view.rho; cb.kx = (view.rho - 1.0f) * view.screenW; cb.ky = (view.rho - 1.0f) * view.screenH; cb.only = (float)view.only;
    cb.scale[0] = (float)eyeW / (float)w[0]; cb.scale[1] = (float)eyeH / (float)h[0];
    auto sized = [&](int k) { cb.size[0] = (float)w[k]; cb.size[1] = (float)h[k]; };

    const bool match = newPicture || !m_haveMaps;
    if (match)
    {
    // 1. The levels.
    sized(0); cb.srcSize[0] = (float)eyeW; cb.srcSize[1] = (float)eyeH;
    Pass(m_psDownFirst, m_tmp[0], cb, src);
    Pass(m_psGrad, m_lvl[0], cb, m_tmp[0].srv);
    for (int k = 1; k < kLevels; ++k)
    {
        sized(k); cb.srcSize[0] = (float)w[k - 1]; cb.srcSize[1] = (float)h[k - 1];
        Pass(m_psDownNext, m_tmp[k], cb, m_tmp[k - 1].srv);
        Pass(m_psGrad, m_lvl[k], cb, m_tmp[k].srv);
    }
    }
    mark(1);

    // 2. Disparity, coarse to fine. After each size: checked, filled, cleaned.
    const Tex* result[kLevels] = {};
    for (int k = 0; k < kLevels; ++k) result[k] = &m_dispB[k];
    if (match)
    {
    for (int k = kLevels - 1; k >= 0; --k)
    {
        sized(k);
        cb.range = std::ceil(m_maxShare * (float)w[k]) + 1.0f;
        // (7x7 at the two smaller sizes; 5x5 at 1/8, where the window is most of the cost.)
        cb.winR = k == 0 ? 2.0f : 3.0f;
        if (k == kLevels - 1) Pass(m_psMatchFull, m_dispA[k], cb, m_lvl[k].srv);
        else                  Pass(m_psMatchRefine, m_dispA[k], cb, m_lvl[k].srv, result[k + 1]->srv);
        Pass(m_psCheck, m_dispB[k], cb, m_dispA[k].srv);
        Pass(m_psFill, m_dispA[k], cb, m_dispB[k].srv);
        Pass(m_psMedian, m_dispB[k], cb, m_dispA[k].srv);
        result[k] = &m_dispB[k];
        // (At the finest size: the edges drawn in, twice -- PSSpread.)
        if (k == 0 && m_spread > 0)
        {
            cb.winR = 1.0f;
            for (int it = 0; it < m_spread; ++it)
            {
                Pass(m_psSpread, m_dispA[k], cb, m_lvl[k].srv, m_dispB[k].srv);
                Pass(m_psSpread, m_dispB[k], cb, m_lvl[k].srv, m_dispA[k].srv);
            }
            // (... and the single texels that a 3x3 window's noise moved, put back.)
            Pass(m_psMedian, m_dispA[k], cb, m_dispB[k].srv);
            Pass(m_psMedian, m_dispB[k], cb, m_dispA[k].srv);
        }
    }
    m_haveMaps = true;
    }
    mark(2);

    // 3. The geometry thrown forward: both eyes' texels, into each new view's half.
    {
        sized(0);
        cb.range = std::ceil(m_maxShare * (float)w[0]) + 1.0f;
        const float zero[4] = {};
        m_context->ClearRenderTargetView(m_target.rtv, zero);
        m_context->ClearDepthStencilView(m_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        m_context->OMSetRenderTargets(1, &m_target.rtv, m_dsv);
        m_context->OMSetDepthStencilState(m_depthOn, 0);
        m_context->VSSetShader(m_vsSplat, nullptr, 0);
        m_context->PSSetShader(m_psSplat, nullptr, 0);
        m_context->VSSetShaderResources(0, 1, &result[0]->srv);
        for (int eye = 0; eye < 2; ++eye)
        {
            cb.eye = (float)eye;
            m_context->UpdateSubresource(m_cb, 0, nullptr, &cb, 0, 0);
            D3D11_VIEWPORT vp{}; vp.TopLeftX = (FLOAT)(eye * w[0]); vp.Width = (FLOAT)w[0]; vp.Height = (FLOAT)h[0]; vp.MaxDepth = 1.0f;
            m_context->RSSetViewports(1, &vp);
            m_context->Draw((UINT)(6 * w[0] * h[0] * 2), 0);
        }
        ID3D11ShaderResourceView* nul = nullptr;
        m_context->VSSetShaderResources(0, 1, &nul);
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        m_context->OMSetDepthStencilState(nullptr, 0);
        Pass(m_psHole, m_targetFilled, cb, m_target.srv);
    }
    mark(3);

    // 4. The new views, at full size; then what neither eye saw.
    cb.size[0] = (float)eyeW; cb.size[1] = (float)eyeH; cb.srcSize[0] = (float)w[0]; cb.srcSize[1] = (float)h[0];
    Pass(m_psCompose, m_out[0], cb, src, result[0]->srv, m_targetFilled.srv);
    Pass(m_psUnseen, m_out[1], cb, m_out[0].srv, nullptr, m_targetFilled.srv);
    mark(4);
    if (m_timing) { m_context->End(m_disjoint); m_timePending = true; m_timing = false; }
    m_context->RSSetState(nullptr);
    return true;
}

bool LookAround::TakeTimes(double ms[4])
{
    if (!m_haveTimes) return false;
    for (int k = 0; k < 4; ++k) ms[k] = m_ms[k];
    m_haveTimes = false;
    return true;
}
