// LookAround -- new views of a scene from two real ones (shaders/Look.hlsl).
//
// Takes the converter's output -- the two eyes side by side, whatever layout
// they came in -- and gives the same back as seen from somewhere else: the left
// eye from position tL, the right from tR, where the left eye's own place is 0
// and the right's is 1 (so tL = 0, tR = 1 is the picture as it was). Moving
// both by the same amount is the viewer moving their head sideways.
//
// Nothing is learned and nothing is known but the two pictures: disparity is
// matched between them, the geometry is thrown forward to the new place, and
// every new pixel's colour is looked up in the real eyes at full size. See
// Look.hlsl for the passes, tools/splatlab for the measurements behind them.
#pragma once

#include "Common.h"
#include <d3d11.h>

namespace srw
{
    class LookAround
    {
    public:
        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
        void Shutdown();

        // src: the two eyes side by side, each eyeW x eyeH, read as linear light
        // (an _SRGB view of 8-bit, or float). False: nothing drawn (use src).
        bool Render(ID3D11ShaderResourceView* src, int eyeW, int eyeH, float tL, float tR);

        ID3D11ShaderResourceView* OutputSRV() const { return m_out[1].srv; }
        ID3D11Texture2D*          OutputTexture() const { return m_out[1].tex; }
        DXGI_FORMAT               OutputFormat() const { return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
        int OutputPerEyeWidth() const { return m_eyeW; }
        int OutputHeight() const { return m_eyeH; }

        // The largest disparity looked for, as a share of the eye's width (0.06:
        // more than any comfortable stereo picture has).
        void SetMaxDisparity(float share) { m_maxShare = share; }

        // GPU time of the last timed frame, by stage (ms): the levels, the
        // matching, the geometry, the new views; false while none is in.
        bool TakeTimes(double ms[4]);

    private:
        struct Tex { ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr; ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; };
        bool Ensure(Tex& t, int w, int h, DXGI_FORMAT fmt, DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN);
        void Release(Tex& t);
        struct CB { float size[2]; float srcSize[2]; float range, tL, tR, eye; float winR, pad; float scale[2]; };
        void Pass(ID3D11PixelShader* ps, const Tex& target, const CB& cb, ID3D11ShaderResourceView* t0,
                  ID3D11ShaderResourceView* t1 = nullptr, ID3D11ShaderResourceView* t2 = nullptr);

        ID3D11Device*        m_device  = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        ID3D11VertexShader*  m_vsFull = nullptr, *m_vsSplat = nullptr;
        ID3D11PixelShader*   m_psDownFirst = nullptr, *m_psDownNext = nullptr, *m_psGrad = nullptr, *m_psMatchFull = nullptr,
                            *m_psMatchRefine = nullptr, *m_psCheck = nullptr, *m_psFill = nullptr, *m_psMedian = nullptr,
                            *m_psSplat = nullptr, *m_psHole = nullptr, *m_psCompose = nullptr, *m_psUnseen = nullptr;
        ID3D11Buffer*            m_cb = nullptr;
        ID3D11SamplerState*      m_sampler = nullptr;
        ID3D11DepthStencilState* m_depthOn = nullptr;
        ID3D11RasterizerState*   m_raster = nullptr;

        static constexpr int kLevels = 3;            // 1/8, 1/16, 1/32 of the eye
        Tex m_tmp[kLevels], m_lvl[kLevels];          // colour; colour + slope
        Tex m_dispA[kLevels], m_dispB[kLevels];      // disparity, two to go between
        Tex m_target, m_targetFilled;                // the new views' disparity
        ID3D11Texture2D*        m_depthTex = nullptr;
        ID3D11DepthStencilView* m_dsv = nullptr;
        Tex m_out[2];                                // the new views: composed, then filled
        int   m_eyeW = 0, m_eyeH = 0;
        float m_maxShare = 0.06f;

        // (Timing: every so many frames, five timestamps.)
        ID3D11Query* m_disjoint = nullptr; ID3D11Query* m_ts[5] = {};
        bool m_timing = false, m_timePending = false; int m_frame = 0;
        double m_ms[4] = {}; bool m_haveTimes = false;
    };
}
