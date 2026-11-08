// OutputEncoder.cpp -- see OutputEncoder.h.
#include "OutputEncoder.h"

#include <d3dcompiler.h>
#include <cstring>

namespace srw
{
namespace
{
    // One triangle over the viewport; uv 0..1 across it. Mode: 0 anaglyph, 1 side by
    // side, 2 top and bottom, 3 rows, 4 columns, 5 checkerboard, 6 HDMI frame packing. Each eye is looked up
    // at uv * scale + offset (its half of the side-by-side picture).
    const char kHLSL[] = R"(
cbuffer C : register(b0)
{
    float4 uvL;  float4 uvR;
    float4 ml0;  float4 ml1;  float4 ml2;
    float4 mr0;  float4 mr1;  float4 mr2;
    float4 misc;            // x mode, y / z the render target's origin on the display
};
Texture2D    t : register(t0);
SamplerState s : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V VSMain(uint id : SV_VertexID)
{
    V o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 Eye(bool right, float2 uv)
{
    const float4 k = right ? uvR : uvL;
    return t.SampleLevel(s, saturate(uv) * k.xy + k.zw, 0);
}
float4 PSMain(V i) : SV_Target
{
    const int mode = (int)misc.x;
    const int2 p = int2(i.pos.xy + misc.yz);   // (the display's own pixel)
    if (mode == 0)
    {
        const float3 l = Eye(false, i.uv).rgb, r = Eye(true, i.uv).rgb;
        const float3 c = float3(dot(ml0.xyz, l), dot(ml1.xyz, l), dot(ml2.xyz, l))
                       + float3(dot(mr0.xyz, r), dot(mr1.xyz, r), dot(mr2.xyz, r));
        return float4(saturate(c), 1);
    }
    if (mode == 1) return float4(Eye(i.uv.x >= 0.5, float2(frac(i.uv.x * 2), i.uv.y)).rgb, 1);
    if (mode == 2) return float4(Eye(i.uv.y >= 0.5, float2(i.uv.x, frac(i.uv.y * 2))).rgb, 1);
    if (mode == 6)
    {
        // HDMI 1.4 frame packing: the left eye in the top 1080 of 2205 rows (720 of
        // 1470), 45 (30) blank, the right eye below -- the same shares at either size.
        const float eye = 1080.0 / 2205.0, gap = 45.0 / 2205.0;
        if (i.uv.y < eye)       return float4(Eye(false, float2(i.uv.x, i.uv.y / eye)).rgb, 1);
        if (i.uv.y < eye + gap) return float4(0, 0, 0, 1);
        return float4(Eye(true, float2(i.uv.x, (i.uv.y - eye - gap) / eye)).rgb, 1);
    }
    const int odd = mode == 3 ? (p.y & 1) : mode == 4 ? (p.x & 1) : ((p.x + p.y) & 1);
    return float4(Eye(odd != 0, i.uv).rgb, 1);
}
)";

    // Anaglyph: the 3 x 3 matrices that take each eye's colour to the output's.
    // Glasses' colours: 0 red / cyan, 1 green / magenta, 2 amber / blue. Style:
    // 0 Optimised (Dubois' least-squares matrices, red / cyan; the others Half
    // Colour), 1 Colour (each eye's own channels as they are), 2 Half Colour (an
    // eye seen through one channel gets its brightness there, not that channel
    // alone -- less rivalry), 3 Grey.
    void AnaglyphMatrices(int colours, int style, float ml[9], float mr[9])
    {
        static const float Y[3] = { 0.299f, 0.587f, 0.114f };
        static const float kDuboisL[9] = { 0.456f, 0.500f, 0.176f,  -0.040f, -0.038f, -0.016f,  -0.015f, -0.021f, -0.005f };
        static const float kDuboisR[9] = { -0.043f, -0.088f, -0.002f,  0.378f, 0.734f, -0.018f,  -0.072f, -0.113f, 1.226f };
        if (colours == 0 && style == 0)
        {
            memcpy(ml, kDuboisL, sizeof(kDuboisL)); memcpy(mr, kDuboisR, sizeof(kDuboisR));
            return;
        }
        if (style == 0) style = 2;
        static const int kMask[3][2][3] = {
            { { 1, 0, 0 }, { 0, 1, 1 } },   // red / cyan
            { { 0, 1, 0 }, { 1, 0, 1 } },   // green / magenta
            { { 1, 1, 0 }, { 0, 0, 1 } },   // amber / blue
        };
        const int (&mk)[2][3] = kMask[colours >= 0 && colours < 3 ? colours : 0];
        for (int eye = 0; eye < 2; ++eye)
        {
            float* m = eye ? mr : ml;
            memset(m, 0, sizeof(float) * 9);
            const int* on = mk[eye];
            const int n = on[0] + on[1] + on[2];
            for (int c = 0; c < 3; ++c)
            {
                if (!on[c]) continue;
                for (int k = 0; k < 3; ++k)
                {
                    const float own = (k == c) ? 1.0f : 0.0f;
                    float v = own;
                    if (style == 3 || (style == 2 && n == 1)) v = Y[k];
                    else if (style == 2)                     v = 0.5f * Y[k] + 0.5f * own;
                    m[c * 3 + k] = v;
                }
            }
        }
    }
}

bool OutputEncoder::Create(ID3D11DeviceContext* context)
{
    Release();
    if (!context) return false;
    m_ctx = context;
    ID3D11Device* dev = nullptr;
    context->GetDevice(&dev);
    if (!dev) return false;
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    bool ok = SUCCEEDED(D3DCompile(kHLSL, sizeof(kHLSL) - 1, "OutputEncoder", nullptr, nullptr, "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err))
           && SUCCEEDED(D3DCompile(kHLSL, sizeof(kHLSL) - 1, "OutputEncoder", nullptr, nullptr, "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err));
    if (!ok && err) Log("OutputEncoder: shader compile failed: %s", (const char*)err->GetBufferPointer());
    ok = ok && SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_vs))
            && SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_ps));
    SAFE_RELEASE(vsb); SAFE_RELEASE(psb); SAFE_RELEASE(err);
    if (ok)
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(m_cbData); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &m_cb));
    }
    if (ok)
    {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(dev->CreateSamplerState(&sd, &m_smp));
    }
    dev->Release();
    if (!ok) { Release(); return false; }
    m_dirty = true;
    return true;
}

void OutputEncoder::Release()
{
    SAFE_RELEASE(m_vs); SAFE_RELEASE(m_ps); SAFE_RELEASE(m_cb); SAFE_RELEASE(m_smp);
    m_srv = nullptr;
}

void OutputEncoder::SetOptions(DisplayOutput output, int anaColours, int anaStyle, bool swapEyes)
{
    if (output == DisplayOutput::Zondision) output = DisplayOutput::SideBySide;
    if (output == m_output && anaColours == m_anaColours && anaStyle == m_anaStyle && swapEyes == m_swap) return;
    m_output = output; m_anaColours = anaColours; m_anaStyle = anaStyle; m_swap = swapEyes;
    m_dirty = true;
}

void OutputEncoder::SetInput(ID3D11ShaderResourceView* srv, int perEyeWidth, int height)
{
    if (srv == m_srv && perEyeWidth == m_eyeW && height == m_h) return;
    m_srv = srv; m_eyeW = perEyeWidth; m_h = height;
    m_dirty = true;
}

void OutputEncoder::Fill()
{
    // (Each eye's place in the input texture, which may be larger than the picture.)
    float texW = (float)(m_eyeW * 2), texH = (float)m_h;
    if (m_srv)
    {
        ID3D11Resource* res = nullptr; m_srv->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)))
        {
            D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
            texW = (float)td.Width; texH = (float)td.Height;
        }
        SAFE_RELEASE(tex); SAFE_RELEASE(res);
    }
    if (texW <= 0.0f || texH <= 0.0f) return;
    const float sx = (float)m_eyeW / texW, sy = (float)m_h / texH;
    float* c = m_cbData;
    memset(c, 0, sizeof(m_cbData));
    float l[4] = { sx, sy, 0.0f, 0.0f }, r[4] = { sx, sy, sx, 0.0f };
    if (m_swap) { float t[4]; memcpy(t, l, sizeof(t)); memcpy(l, r, sizeof(l)); memcpy(r, t, sizeof(r)); }
    memcpy(c + 0, l, sizeof(l));
    memcpy(c + 4, r, sizeof(r));
    float ml[9], mr[9];
    AnaglyphMatrices(m_anaColours, m_anaStyle, ml, mr);
    for (int row = 0; row < 3; ++row)
        for (int k = 0; k < 3; ++k) { c[8 + row * 4 + k] = ml[row * 3 + k]; c[20 + row * 4 + k] = mr[row * 3 + k]; }
    int mode = 0;
    switch (m_output)
    {
    case DisplayOutput::SideBySide:        mode = 1; break;
    case DisplayOutput::TopAndBottom:      mode = 2; break;
    case DisplayOutput::RowInterleaved:    mode = 3; break;
    case DisplayOutput::ColumnInterleaved: mode = 4; break;
    case DisplayOutput::Checkerboard:      mode = 5; break;
    case DisplayOutput::FramePacking:      mode = 6; break;
    case DisplayOutput::Vision3D:          mode = 1; break;   // (3D Vision: side by side, handed to the driver per eye)
    default:                               mode = 0; break;
    }
    c[32] = (float)mode;
}

void OutputEncoder::Draw(int originX, int originY)
{
    if (!m_ps || !m_srv || !m_ctx) return;
    if (m_dirty) { Fill(); m_dirty = false; }
    m_cbData[33] = (float)originX; m_cbData[34] = (float)originY;
    m_ctx->UpdateSubresource(m_cb, 0, nullptr, m_cbData, 0, 0);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_vs, nullptr, 0);
    m_ctx->GSSetShader(nullptr, nullptr, 0);
    m_ctx->PSSetShader(m_ps, nullptr, 0);
    m_ctx->PSSetConstantBuffers(0, 1, &m_cb);
    m_ctx->PSSetSamplers(0, 1, &m_smp);
    m_ctx->PSSetShaderResources(0, 1, &m_srv);
    m_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_ctx->OMSetDepthStencilState(nullptr, 0);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    m_ctx->PSSetShaderResources(0, 1, &none);
}
}
