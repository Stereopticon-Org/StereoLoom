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
#include "Look_PSFine.h"
#include "Look_PSGuide.h"
#include "Look_PSFuse.h"
#include "Look_PSDilate.h"
#include "Look_PSSplat.h"
#include "Look_PSHole.h"
#include "Look_PSCompose.h"
#include "Look_PSUnseen.h"
#include "Look_PSPlate.h"
#include "Look_PSFillDown.h"
#include "Look_PSShrink.h"
#include "Look_PSRight.h"
#include "Look_PSRightFill.h"
#include "Look_PSSnap.h"
#include "Look_PSLKSum.h"
#include "Look_PSLKSolve.h"
#include "Look_VSGauss.h"
#include "Look_PSGauss.h"
#include "Look_PSGaussResolve.h"

using namespace srw;

bool LookAround::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!device || !context) return false;
    m_device = device; m_context = context;
    HRESULT hr = device->CreateVertexShader(g_Look_VSFull, sizeof(g_Look_VSFull), nullptr, &m_vsFull);
    if (SUCCEEDED(hr)) hr = device->CreateVertexShader(g_Look_VSSplat, sizeof(g_Look_VSSplat), nullptr, &m_vsSplat);
    if (SUCCEEDED(hr)) hr = device->CreateVertexShader(g_Look_VSGauss, sizeof(g_Look_VSGauss), nullptr, &m_vsGauss);
    {
        // (The Gaussians' weights and weighted colours added up.)
        D3D11_BLEND_DESC bl{};
        bl.RenderTarget[0].BlendEnable = TRUE; bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (SUCCEEDED(hr)) hr = device->CreateBlendState(&bl, &m_blendAdd);
    }
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
        { g_Look_PSFine,        sizeof(g_Look_PSFine),        &m_psFine },
        { g_Look_PSGuide,       sizeof(g_Look_PSGuide),       &m_psGuide },
        { g_Look_PSFuse,        sizeof(g_Look_PSFuse),        &m_psFuse },
        { g_Look_PSDilate,      sizeof(g_Look_PSDilate),      &m_psDilate },
        { g_Look_PSSplat,       sizeof(g_Look_PSSplat),       &m_psSplat },
        { g_Look_PSHole,        sizeof(g_Look_PSHole),        &m_psHole },
        { g_Look_PSCompose,     sizeof(g_Look_PSCompose),     &m_psCompose },
        { g_Look_PSUnseen,      sizeof(g_Look_PSUnseen),      &m_psUnseen },
        { g_Look_PSPlate,       sizeof(g_Look_PSPlate),       &m_psPlate },
        { g_Look_PSFillDown,    sizeof(g_Look_PSFillDown),    &m_psFillDown },
        { g_Look_PSShrink,      sizeof(g_Look_PSShrink),      &m_psShrink },
        { g_Look_PSRight,       sizeof(g_Look_PSRight),       &m_psRight },
        { g_Look_PSRightFill,   sizeof(g_Look_PSRightFill),   &m_psRightFill },
        { g_Look_PSSnap,        sizeof(g_Look_PSSnap),        &m_psSnap },
        { g_Look_PSLKSum,       sizeof(g_Look_PSLKSum),       &m_psLKSum },
        { g_Look_PSLKSolve,     sizeof(g_Look_PSLKSolve),     &m_psLKSolve },
        { g_Look_PSGauss,       sizeof(g_Look_PSGauss),       &m_psGauss },
        { g_Look_PSGaussResolve, sizeof(g_Look_PSGaussResolve), &m_psGaussResolve },
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
    Release(m_plate[0]); Release(m_plate[1]); Release(m_extCopy); Release(m_extPrep); Release(m_small);
    for (int k = 0; k < kFillMax; ++k) { SAFE_RELEASE(m_fillRtv[k]); SAFE_RELEASE(m_fillMip[k]); }
    SAFE_RELEASE(m_fillAll); SAFE_RELEASE(m_fillTex); m_fillMips = 0;
    for (int k = 0; k < 4; ++k) Release(m_lkSum[k]);
    Release(m_est[0]); Release(m_est[1]); Release(m_estQ); SAFE_RELEASE(m_estRead);
    for (int k = 0; k < kLevels; ++k) Release(m_prevTmp[k]);
    SAFE_RELEASE(m_psLKSum); SAFE_RELEASE(m_psLKSolve);
    for (int k = 0; k < kLevels; ++k) { Release(m_tmp[k]); Release(m_lvl[k]); Release(m_dispA[k]); Release(m_dispB[k]); }
    Release(m_target); Release(m_targetFilled); Release(m_out[0]); Release(m_out[1]);
    SAFE_RELEASE(m_dsv); SAFE_RELEASE(m_depthTex);
    SAFE_RELEASE(m_disjoint); for (auto& q : m_ts) SAFE_RELEASE(q);
    SAFE_RELEASE(m_raster); SAFE_RELEASE(m_depthOn); SAFE_RELEASE(m_sampler); SAFE_RELEASE(m_cb);
    SAFE_RELEASE(m_psRight); SAFE_RELEASE(m_psRightFill); SAFE_RELEASE(m_psSnap); SAFE_RELEASE(m_psShrink); SAFE_RELEASE(m_psFillDown); SAFE_RELEASE(m_psPlate); SAFE_RELEASE(m_psUnseen); SAFE_RELEASE(m_psCompose); SAFE_RELEASE(m_psHole); SAFE_RELEASE(m_psSplat);
    SAFE_RELEASE(m_psSpread); SAFE_RELEASE(m_psFine); SAFE_RELEASE(m_psGuide); SAFE_RELEASE(m_psFuse); SAFE_RELEASE(m_psDilate);
    SAFE_RELEASE(m_psMedian); SAFE_RELEASE(m_psFill); SAFE_RELEASE(m_psCheck); SAFE_RELEASE(m_psMatchRefine);
    SAFE_RELEASE(m_psMatchFull); SAFE_RELEASE(m_psGrad); SAFE_RELEASE(m_psDownNext); SAFE_RELEASE(m_psDownFirst);
    SAFE_RELEASE(m_vsSplat); SAFE_RELEASE(m_vsFull);
    SAFE_RELEASE(m_vsGauss); SAFE_RELEASE(m_psGauss); SAFE_RELEASE(m_psGaussResolve); SAFE_RELEASE(m_blendAdd); Release(m_acc); Release(m_cover);
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
    const int autoBase = eyeW >= 2600 ? 8 : eyeW >= 1300 ? 4 : 2;
    // (m_base: the finest size asked for -- 1 the eye's own. A level more for each halving, so the smallest stays as small.)
    const int base = m_base > 0 ? (std::min)(m_base, autoBase) : autoBase;
    int levels = 4; for (int b = base; b < autoBase; b *= 2) ++levels;
    int w[kLevels], h[kLevels];
    w[0] = eyeW / base; h[0] = eyeH / base;
    for (int k = 1; k < levels; ++k) { w[k] = (std::max)(1, w[k - 1] / 2); h[k] = (std::max)(1, h[k - 1] / 2); }
    bool ok = true;
    for (int k = 0; k < levels && ok; ++k)
        ok = Ensure(m_tmp[k], 2 * w[k], h[k], DXGI_FORMAT_R16G16B16A16_FLOAT) && Ensure(m_lvl[k], 2 * w[k], h[k], DXGI_FORMAT_R16G16B16A16_FLOAT) &&
             Ensure(m_dispA[k], 2 * w[k], h[k], DXGI_FORMAT_R16G16B16A16_FLOAT) && Ensure(m_dispB[k], 2 * w[k], h[k], DXGI_FORMAT_R16G16B16A16_FLOAT);
    ok = ok && Ensure(m_target, 2 * w[0], h[0], DXGI_FORMAT_R16G16_FLOAT) && Ensure(m_targetFilled, 2 * w[0], h[0], DXGI_FORMAT_R16G16_FLOAT) &&
         Ensure(m_out[0], 2 * eyeW, eyeH, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) &&
         Ensure(m_out[1], 2 * eyeW, eyeH, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    // (The plate: the eye's picture and an eighth of it more on every side.)
    const int padX = eyeW / 8, padY = eyeH / 8, plateW = 2 * (eyeW + 2 * padX), plateH = eyeH + 2 * padY;
    if (ok && (!m_plate[0].tex || m_plate[0].w != plateW || m_plate[0].h != plateH)) m_plateHave = false;
    ok = ok && Ensure(m_plate[0], plateW, plateH, DXGI_FORMAT_R16G16B16A16_FLOAT) && Ensure(m_plate[1], plateW, plateH, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (int k = 0; k < levels && ok; ++k)
    {
        if (!m_prevTmp[k].tex || m_prevTmp[k].w != 2 * w[k] || m_prevTmp[k].h != h[k]) m_prevHave = false;
        ok = Ensure(m_prevTmp[k], 2 * w[k], h[k], DXGI_FORMAT_R16G16B16A16_FLOAT);
    }
    for (int k = 0; k < 4 && ok; ++k) ok = Ensure(m_lkSum[k], 16, 16, DXGI_FORMAT_R32G32B32A32_FLOAT);
    ok = ok && Ensure(m_est[0], 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT) && Ensure(m_est[1], 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT) && Ensure(m_estQ, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT);
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
    cb.pad[0] = (float)padX; cb.pad[1] = (float)padY;
    cb.ty = view.ty; cb.rho = view.rho; cb.kx = (view.rho - 1.0f) * view.screenW; cb.ky = (view.rho - 1.0f) * view.screenH; cb.only = (float)view.only; for (int q = 0; q < 4; ++q) cb.opt[q] = m_opt[q];
    // (With a trained matcher's depth alone the outlines are where they should be: the search of surfaces a
    // little way off, made for the windows' fat ones, finds next to nothing more -- 0.05 dB -- and is a third of a frame's cost.)
    if (m_extDisp && m_extStage == 2) { cb.opt[0] = m_extSoft < -0.5f ? 2.0f : 0.0f; cb.opt[2] = m_extSoft; }   // (... its outlines by the edge strip where that is on -- the four a little way off are then read, for the far side and the slope -- else not mixed across)
    cb.scale[0] = (float)eyeW / (float)w[0]; cb.scale[1] = (float)eyeH / (float)h[0];
    auto sized = [&](int k) { cb.size[0] = (float)w[k]; cb.size[1] = (float)h[k]; };

    const bool match = newPicture || !m_haveMaps;
    // (A trained matcher's depth alone, and no plate kept: the look-around's own levels and its own matching at
    // the smaller sizes feed nothing -- not made. They were two fifths of a frame's cost.)
    const bool extOnly = m_extDisp && m_extStage == 2;
    const bool ownLevels = !extOnly || m_plateOn;
    if (match && ownLevels)
    {
    // 1. The levels.
    sized(0); cb.srcSize[0] = (float)eyeW; cb.srcSize[1] = (float)eyeH;
    Pass(m_psDownFirst, m_tmp[0], cb, src);
    Pass(m_psGrad, m_lvl[0], cb, m_tmp[0].srv);
    for (int k = 1; k < levels; ++k)
    {
        sized(k); cb.srcSize[0] = (float)w[k - 1]; cb.srcSize[1] = (float)h[k - 1];
        Pass(m_psDownNext, m_tmp[k], cb, m_tmp[k - 1].srv);
        Pass(m_psGrad, m_lvl[k], cb, m_tmp[k].srv);
    }
    }
    mark(1);

    // 2. Disparity, coarse to fine. After each size: checked, filled, cleaned.
    const Tex* result[kLevels] = {};
    for (int k = 0; k < levels; ++k) result[k] = &m_dispB[k];
    if (m_fine) result[0] = &m_dispA[0];
    if (match)
    {
    for (int k = levels - 1; k >= 0; --k)
    {
        if (k > 0 && !ownLevels) continue;
        sized(k);
        cb.range = std::ceil(m_maxShare * (float)w[k]) + 1.0f;
        // (7x7 at the two smaller sizes; 5x5 at 1/8, where the window is most of the cost.)
        cb.winR = k == 0 ? 2.0f : 3.0f;
        // (The other matcher's depth alone: the finest size's own matching is not wanted, and not done.)
        const bool own = !(k == 0 && m_extDisp && m_extStage == 2);
        if (own) {
        if (k == levels - 1) Pass(m_psMatchFull, m_dispA[k], cb, m_lvl[k].srv);
        else                  Pass(m_psMatchRefine, m_dispA[k], cb, m_lvl[k].srv, result[k + 1]->srv);
        // (A disparity from elsewhere -- another matcher's, under trial: it takes the place of the one
        // found, and is checked, cleaned, drawn in at its edges and made finer as that would be.)
        if (k == 0 && m_extDisp && m_extStage == 0) m_context->CopyResource(m_dispA[0].tex, m_extDisp);
        Pass(m_psCheck, m_dispB[k], cb, m_dispA[k].srv);
        Pass(m_psFill, m_dispA[k], cb, m_dispB[k].srv);
        Pass(m_psMedian, m_dispB[k], cb, m_dispA[k].srv);
        result[k] = &m_dispB[k];
        if (k == 0 && m_extDisp && m_extStage == 1) m_context->CopyResource(m_dispB[0].tex, m_extDisp);   // (after the checking and cleaning, before the edges)
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
        result[k] = &m_dispB[k];
        // (What is made of the other matcher's answer without the picture -- its ramps made steps, the right eye's
        // map from the left's -- is made once for each answer and kept: m_extPrep. A video gives a new picture many
        // times for each answer.)
        const bool prepKept = k == 0 && extOnly && m_extPrepSerial == m_extSerial && m_extPrep.tex && m_extPrep.w == 2 * w[0] && m_extPrep.h == h[0];
        if (prepKept) m_context->CopyResource(m_dispB[0].tex, m_extPrep.tex);
        if (k == 0 && extOnly && !prepKept) m_context->CopyResource(m_dispB[0].tex, m_extDisp);   // (after everything but the making finer)
        // (Its ramps made steps again: PSSnap, in pairs so the answer is left in B.)
        if (k == 0 && extOnly && !prepKept)
            for (int it = 0; it < (m_extSnap + 1) / 2; ++it)
            {
                Pass(m_psSnap, m_dispA[0], cb, nullptr, m_dispB[0].srv);
                Pass(m_psSnap, m_dispB[0], cb, nullptr, m_dispA[0].srv);
            }
        // (... the left eye's only: the right eye's made from it.)
        if (k == 0 && extOnly && !prepKept && m_extLeftOnly)
        {
            Pass(m_psRight, m_dispA[0], cb, nullptr, m_dispB[0].srv);
            Pass(m_psRightFill, m_dispB[0], cb, nullptr, m_dispA[0].srv);
        }
        if (k == 0 && extOnly && !prepKept && Ensure(m_extPrep, 2 * w[0], h[0], DXGI_FORMAT_R16G16B16A16_FLOAT))
        {
            m_context->CopyResource(m_extPrep.tex, m_dispB[0].tex);
            m_extPrepSerial = m_extSerial;
        }
        // (4: the better of it and the look-around's own at each texel: PSFuse.)
        if (k == 0 && m_extDisp && m_extStage == 4 && Ensure(m_extCopy, 2 * w[0], h[0], DXGI_FORMAT_R16G16B16A16_FLOAT))
        {
            m_context->CopyResource(m_extCopy.tex, m_extDisp);
            const float keep = cb.opt[3], keep2 = cb.opt[2], keep0 = cb.opt[0]; cb.opt[3] = m_fuseBias; cb.opt[2] = m_fuseEdge ? 1.0f : 0.0f; cb.opt[0] = m_fuseReach;
            Pass(m_psFuse, m_dispA[0], cb, m_lvl[0].srv, m_extCopy.srv, m_dispB[0].srv);
            cb.opt[3] = keep; cb.opt[2] = keep2; cb.opt[0] = keep0;
            m_context->CopyResource(m_dispB[0].tex, m_dispA[0].tex);
        }
        // (3: ... and first put right in its detail, within two texels of where it was: PSGuide.)
        if (k == 0 && m_extDisp && m_extStage == 3)
        {
            m_context->CopyResource(m_dispA[0].tex, m_extDisp);
            cb.winR = 2.0f;
            Pass(m_psGuide, m_dispB[0], cb, m_lvl[0].srv, m_dispA[0].srv);
        }
        // (... and the answer made finer against the picture at its full size.)
        if (k == 0 && m_fine)
        {
            cb.srcSize[0] = (float)eyeW; cb.srcSize[1] = (float)eyeH; cb.winR = m_fineR;
            Pass(m_psFine, m_dispA[0], cb, src, m_dispB[0].srv);
            result[0] = &m_dispA[0];
        }
        // (With the other matcher's depth: its outlines taken a texel wide -- PSDilate. The answer is
        // left where it was: in A where it was made finer, else in B.)
        if (k == 0 && m_extDisp && m_extDilate > 0)
        {
            sized(0); cb.winR = (float)m_extDilate;
            Tex& at = m_fine ? m_dispA[0] : m_dispB[0]; Tex& other = m_fine ? m_dispB[0] : m_dispA[0];
            Pass(m_psDilate, other, cb, nullptr, at.srv);
            m_context->CopyResource(at.tex, other.tex);
        }
    }
    m_haveMaps = true;
    // How the picture moved since the one before: from the smallest level to the finest, twice at each.
    m_estHave = false;
    if (m_findMotion && m_plateOn && m_prevHave && m_plateHave)
    {
        const float zero4[4] = {};
        m_context->ClearRenderTargetView(m_est[m_estCur].rtv, zero4);
        for (int k = levels - 1; k >= 0; --k)
        for (int it = 0; it < 2; ++it)
        {
            sized(k);
            cb.lk[0] = (float)eyeW / (float)w[k]; cb.lk[1] = 0.03f * (float)eyeW; cb.lk[2] = (float)eyeW / (float)w[k]; cb.lk[3] = 0.0f;
            m_context->UpdateSubresource(m_cb, 0, nullptr, &cb, 0, 0);
            D3D11_VIEWPORT vp{}; vp.Width = 16.0f; vp.Height = 16.0f; vp.MaxDepth = 1.0f;
            ID3D11RenderTargetView* sums[4] = { m_lkSum[0].rtv, m_lkSum[1].rtv, m_lkSum[2].rtv, m_lkSum[3].rtv };
            m_context->OMSetRenderTargets(4, sums, nullptr);
            m_context->RSSetViewports(1, &vp);
            m_context->VSSetShader(m_vsFull, nullptr, 0);
            m_context->PSSetShader(m_psLKSum, nullptr, 0);
            ID3D11ShaderResourceView* in1[5] = { m_tmp[k].srv, m_prevTmp[k].srv, result[k]->srv, m_est[m_estCur].srv, nullptr };
            m_context->PSSetShaderResources(0, 5, in1);
            m_context->Draw(3, 0);
            ID3D11ShaderResourceView* none[5] = {};
            m_context->PSSetShaderResources(0, 5, none);
            vp.Width = 1.0f; vp.Height = 1.0f;
            ID3D11RenderTargetView* outs[2] = { m_est[1 - m_estCur].rtv, m_estQ.rtv };
            m_context->OMSetRenderTargets(2, outs, nullptr);
            m_context->RSSetViewports(1, &vp);
            m_context->PSSetShader(m_psLKSolve, nullptr, 0);
            ID3D11ShaderResourceView* in2[5] = { m_lkSum[0].srv, m_lkSum[1].srv, m_lkSum[2].srv, m_lkSum[3].srv, m_est[m_estCur].srv };
            m_context->PSSetShaderResources(0, 5, in2);
            m_context->Draw(3, 0);
            m_context->PSSetShaderResources(0, 5, none);
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
            m_estCur = 1 - m_estCur;
        }
        m_estHave = true;
    }
    if (ownLevels) { for (int k = 0; k < levels; ++k) m_context->CopyResource(m_prevTmp[k].tex, m_tmp[k].tex); m_prevHave = true; }
    else m_prevHave = false;
    // The plate: the one before moved as the picture moved, and this picture put into it.
    if (m_plateOn)
    {
        cb.size[0] = (float)eyeW; cb.size[1] = (float)eyeH; cb.srcSize[0] = (float)w[0]; cb.srcSize[1] = (float)h[0];
        for (int q = 0; q < 4; ++q) cb.motion[q] = m_motion[q];
        cb.plate[0] = m_plateHave ? 1.0f : 0.0f;
        cb.lk[3] = m_estHave ? 1.0f : 0.0f;
        ID3D11ShaderResourceView* est[2] = { m_estHave ? m_est[m_estCur].srv : nullptr, m_estHave ? m_estQ.srv : nullptr };
        m_context->PSSetShaderResources(3, 2, est);
        Pass(m_psPlate, m_plate[1 - m_plateCur], cb, src, result[0]->srv, m_plate[m_plateCur].srv);
        est[0] = est[1] = nullptr; m_context->PSSetShaderResources(3, 2, est);
        m_plateCur = 1 - m_plateCur; m_plateHave = true;
        for (int q = 0; q < 4; ++q) m_motion[q] = 0.0f;   // (it was this picture's: the same picture given again has not moved)
    }
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
    if (m_gauss > 0 && Ensure(m_acc, 2 * eyeW, eyeH, DXGI_FORMAT_R16G16B16A16_FLOAT) && Ensure(m_cover, 2 * eyeW, eyeH, DXGI_FORMAT_R16_FLOAT))
    {
        // Gaussian splats: every spot of both eyes drawn into each new view's half, added up; then divided out.
        const float zero[4] = {};
        const float step = (float)m_gauss;
        const int gw = (int)std::ceil((float)eyeW / step), gh = (int)std::ceil((float)eyeH / step);
        cb.lk[0] = step; cb.lk[1] = m_gaussSigma; cb.lk[2] = m_gaussSeen; cb.lk[3] = m_gaussEdge ? 1.0f : 0.0f;
        m_context->ClearRenderTargetView(m_acc.rtv, zero);
        m_context->ClearRenderTargetView(m_cover.rtv, zero);
        ID3D11RenderTargetView* both[2] = { m_acc.rtv, m_cover.rtv };
        m_context->OMSetRenderTargets(2, both, nullptr);
        m_context->OMSetBlendState(m_blendAdd, nullptr, 0xFFFFFFFF);
        m_context->VSSetShader(m_vsGauss, nullptr, 0);
        m_context->PSSetShader(m_psGauss, nullptr, 0);
        ID3D11ShaderResourceView* vin[2] = { src, result[0]->srv };
        m_context->VSSetShaderResources(0, 2, vin);
        ID3D11ShaderResourceView* pin[3] = { nullptr, nullptr, m_targetFilled.srv };
        m_context->PSSetShaderResources(0, 3, pin);
        for (int eye = 0; eye < 2; ++eye)
        {
            cb.eye = (float)eye;
            m_context->UpdateSubresource(m_cb, 0, nullptr, &cb, 0, 0);
            // (Each view's spots kept to its own half: the viewport's edge is where they are cut.)
            D3D11_VIEWPORT vp{}; vp.TopLeftX = (FLOAT)(eye * eyeW); vp.Width = (FLOAT)eyeW; vp.Height = (FLOAT)eyeH; vp.MaxDepth = 1.0f;
            m_context->RSSetViewports(1, &vp);
            m_context->Draw((UINT)(6 * gw * gh * 2), 0);
        }
        ID3D11ShaderResourceView* nul3[3] = {};
        m_context->VSSetShaderResources(0, 2, nul3);
        m_context->PSSetShaderResources(0, 3, nul3);
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        Pass(m_psGaussResolve, m_out[0], cb, m_acc.srv, m_cover.srv);
    }
    else
    Pass(m_psCompose, m_out[0], cb, src, result[0]->srv, m_targetFilled.srv);
    // The far side of things, level by level, for what neither eye saw.
    cb.pad[2] = 0.0f;
    if (m_fillOn)
    {
        const int fw = (std::max)(2, eyeW / 2) * 2, fh = (std::max)(1, eyeH / 2);
        if (!m_fillTex || m_fillW != fw || m_fillH != fh)
        {
            for (int k = 0; k < kFillMax; ++k) { SAFE_RELEASE(m_fillRtv[k]); SAFE_RELEASE(m_fillMip[k]); }
            SAFE_RELEASE(m_fillAll); SAFE_RELEASE(m_fillTex); m_fillMips = 0; m_fillW = fw; m_fillH = fh;
            int n = 1; for (int a = fw / 2, b = fh; n < kFillMax && a >= 16 && b >= 8; a /= 2, b /= 2) ++n;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = (UINT)fw; td.Height = (UINT)fh; td.MipLevels = (UINT)n; td.ArraySize = 1; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            bool made = SUCCEEDED(m_device->CreateTexture2D(&td, nullptr, &m_fillTex)) && SUCCEEDED(m_device->CreateShaderResourceView(m_fillTex, nullptr, &m_fillAll));
            for (int k = 0; k < n && made; ++k)
            {
                D3D11_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = td.Format; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; rv.Texture2D.MipSlice = (UINT)k;
                D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = td.Format; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MostDetailedMip = (UINT)k; sv.Texture2D.MipLevels = 1;
                made = SUCCEEDED(m_device->CreateRenderTargetView(m_fillTex, &rv, &m_fillRtv[k])) && SUCCEEDED(m_device->CreateShaderResourceView(m_fillTex, &sv, &m_fillMip[k]));
            }
            if (made) m_fillMips = n;
        }
        int mw = m_fillW, mh = m_fillH;
        for (int k = 0; k < m_fillMips; ++k)
        {
            Tex t; t.rtv = m_fillRtv[k]; t.w = mw; t.h = mh;
            CB c2 = cb; c2.size[0] = (float)(mw / 2); c2.size[1] = (float)mh; c2.eye = k == 0 ? 0.0f : 1.0f;
            Pass(m_psFillDown, t, c2, k == 0 ? m_out[0].srv : m_fillMip[k - 1], nullptr, m_targetFilled.srv);
            mw = (std::max)(1, mw / 2); mh = (std::max)(1, mh / 2);
        }
        cb.pad[2] = (float)m_fillMips;
    }
    cb.plate[1] = (m_plateOn && m_plateHave) ? 1.0f : 0.0f; cb.plate[2] = m_markFill ? 1.0f : 0.0f;
    cb.plate[3] = m_layer ? 1.0f : 0.0f;
    { ID3D11ShaderResourceView* fa[2] = { cb.pad[2] > 0.5f ? m_fillAll : nullptr, m_layer }; m_context->PSSetShaderResources(3, 2, fa); }
    Pass(m_psUnseen, m_out[1], cb, m_out[0].srv, m_plate[m_plateCur].srv, m_targetFilled.srv);
    { ID3D11ShaderResourceView* fa[2] = { nullptr, nullptr }; m_context->PSSetShaderResources(3, 2, fa); }
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

bool LookAround::ReadMotion(float out[6])
{
    if (!m_estHave || !m_est[m_estCur].tex) return false;
    if (!m_estRead)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 2; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_estRead))) return false;
    }
    m_context->CopySubresourceRegion(m_estRead, 0, 0, 0, 0, m_est[m_estCur].tex, 0, nullptr);
    m_context->CopySubresourceRegion(m_estRead, 0, 1, 0, 0, m_estQ.tex, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(m_context->Map(m_estRead, 0, D3D11_MAP_READ, 0, &m))) return false;
    const float* f = (const float*)m.pData;
    for (int k = 0; k < 4; ++k) out[k] = f[k];
    out[4] = f[4]; out[5] = f[5];
    m_context->Unmap(m_estRead, 0);
    return true;
}

void LookAround::MapSize(int eyeW, int eyeH, int& w, int& h) const
{
    const int autoBase = eyeW >= 2600 ? 8 : eyeW >= 1300 ? 4 : 2;
    const int base = m_base > 0 ? (std::min)(m_base, autoBase) : autoBase;
    w = eyeW / base; h = eyeH / base;
}

ID3D11Texture2D* LookAround::Shrink(ID3D11ShaderResourceView* src, int eyeW, int eyeH, int w, int h)
{
    if (!m_context || !src || w < 16 || h < 16 || !Ensure(m_small, 2 * w, h, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) return nullptr;
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(nullptr);
    m_context->RSSetState(m_raster);
    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_context->PSSetSamplers(0, 1, &m_sampler);
    m_context->PSSetConstantBuffers(0, 1, &m_cb);
    m_context->VSSetConstantBuffers(0, 1, &m_cb);
    CB cb{};
    cb.size[0] = (float)w; cb.size[1] = (float)h; cb.srcSize[0] = (float)eyeW; cb.srcSize[1] = (float)eyeH;
    Pass(m_psShrink, m_small, cb, src);
    m_context->RSSetState(nullptr);
    return m_small.tex;
}
