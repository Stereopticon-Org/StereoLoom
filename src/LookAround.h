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
        // Where the new views are from. tL / tR: the left and right eye's new
        // places across (their own are 0 and 1). ty: how far DOWN the screen the
        // head is from the eyes' own height, in eye spacings. rho: the eyes' own
        // distance from the screen over the new one (1: as far; 2: half as far).
        // screenW / screenH: the picture's size on the display in eye spacings
        // (a 27 inch display and 63 mm eyes: 9.5 x 5.3). only: a test -- -1 both
        // eyes are used, 0 or 1 that eye alone.
        struct View { float tL = 0.0f, tR = 1.0f, ty = 0.0f, rho = 1.0f, screenW = 9.5f, screenH = 5.3f; int only = -1; };
        // newPicture false: src is the picture given last time -- only the head has
        // moved: the disparity found then still stands (the levels and the
        // matching, about half the work, are not done again).
        bool Render(ID3D11ShaderResourceView* src, int eyeW, int eyeH, const View& view, bool newPicture = true);
        bool Render(ID3D11ShaderResourceView* src, int eyeW, int eyeH, float tL, float tR) { View v; v.tL = tL; v.tR = tR; return Render(src, eyeW, eyeH, v); }

        ID3D11ShaderResourceView* OutputSRV() const { return m_out[1].srv; }
        ID3D11Texture2D*          OutputTexture() const { return m_out[1].tex; }
        DXGI_FORMAT               OutputFormat() const { return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
        // The disparity found for the two eyes (side by side, .r in texels of its own size): for looking at.
        ID3D11Texture2D* DisparityTexture() const { return m_fine ? m_dispA[0].tex : m_dispB[0].tex; }
        int OutputPerEyeWidth() const { return m_eyeW; }
        int OutputHeight() const { return m_eyeH; }

        // The largest disparity looked for, as a share of the eye's width (0.25:
        // further than anything can pop out and still be one picture -- a smaller limit showed as a wall things stopped at).
        void SetMaxDisparity(float share) { m_maxShare = share; }
        void SetOpt(int i, float v) { if (i >= 0 && i < 4) m_opt[i] = v; }   // (switches under trial: see g_opt in Look.hlsl)
        // The plate (what was in sight in earlier pictures, for filling what is hidden now): off or on;
        // forgotten (a new scene); and how the picture has moved since the one before, in its own
        // pixels -- x, y for everything -- and perD times its own disparity more (the camera a tenth of an
        // eye spacing to the side: 0.1). It is for the next new picture only.
        void SetPlate(bool on) { m_plateOn = on; }
        void ForgetPlate() { m_plateHave = false; }
        void SetMarkFill(bool on) { m_markFill = on; }   // (a test: what the plate filled drawn magenta, what it could not green)
        // The motion between pictures found from the pictures themselves (on, as a rule; off: only what
        // SetMotion gives). ReadMotion: what was found for the last picture -- x, y, per-disparity x, y,
        // how badly it fitted, the share of the picture that counted. It waits for the GPU: for tests.
        void SetFindMotion(bool on) { m_findMotion = on; }
        bool ReadMotion(float out[6]);
        void SetMotion(float x, float y, float perDx, float perDy) { m_motion[0] = x; m_motion[1] = y; m_motion[2] = perDx; m_motion[3] = perDy; }
        // A trial: a disparity from another matcher in place of the one found here. tex: both eyes side by
        // side at MapSize (R16G16B16A16_FLOAT, .r in that map's texels; the left eye's point is at x - d in
        // the right, the right's at x + d in the left). nullptr: its own again.
        void SetExternalDisparity(ID3D11Texture2D* tex) { m_extDisp = tex; m_haveMaps = false; }
        void MapSize(int eyeW, int eyeH, int& w, int& h) const;
        void SetBase(int base) { m_base = base; m_haveMaps = false; }   // (the finest matching size: 1 the eye itself, 2 half, ...; 0 as it sees fit)
        // How many times the edges are drawn in at the finest size (pairs of passes; 0: not). A test switch.
        void SetSpread(int n) { m_spread = n; m_haveMaps = false; }
        void SetFine(int taps) { m_fine = taps > 0; m_fineR = (float)(taps > 0 ? taps : 1); m_haveMaps = false; }   // (0 off; else the window: 1 3x3, 2 5x5)

        // GPU time of the last timed frame, by stage (ms): the levels, the
        // matching, the geometry, the new views; false while none is in.
        bool TakeTimes(double ms[4]);

    private:
        struct Tex { ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr; ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; };
        bool Ensure(Tex& t, int w, int h, DXGI_FORMAT fmt, DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN);
        void Release(Tex& t);
        struct CB { float size[2]; float srcSize[2]; float range, tL, tR, eye; float winR, ty; float scale[2]; float rho, kx, ky, only; float opt[4]; float motion[4]; float plate[4]; float pad[4]; float lk[4]; };
        void Pass(ID3D11PixelShader* ps, const Tex& target, const CB& cb, ID3D11ShaderResourceView* t0,
                  ID3D11ShaderResourceView* t1 = nullptr, ID3D11ShaderResourceView* t2 = nullptr);

        ID3D11Device*        m_device  = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        ID3D11VertexShader*  m_vsFull = nullptr, *m_vsSplat = nullptr;
        ID3D11PixelShader*   m_psDownFirst = nullptr, *m_psDownNext = nullptr, *m_psGrad = nullptr, *m_psMatchFull = nullptr,
                            *m_psMatchRefine = nullptr, *m_psCheck = nullptr, *m_psFill = nullptr, *m_psMedian = nullptr, *m_psSpread = nullptr, *m_psFine = nullptr,
                            *m_psSplat = nullptr, *m_psHole = nullptr, *m_psCompose = nullptr, *m_psUnseen = nullptr, *m_psPlate = nullptr, *m_psLKSum = nullptr, *m_psLKSolve = nullptr;
        ID3D11Buffer*            m_cb = nullptr;
        ID3D11SamplerState*      m_sampler = nullptr;
        ID3D11DepthStencilState* m_depthOn = nullptr;
        ID3D11RasterizerState*   m_raster = nullptr;

        static constexpr int kLevels = 7;            // the most levels: four as a rule (the eye at 1/base and halved three times), more when a finer base is asked for
        Tex m_tmp[kLevels], m_lvl[kLevels];          // colour; colour + slope
        Tex m_dispA[kLevels], m_dispB[kLevels];      // disparity, two to go between
        Tex m_target, m_targetFilled;                // the new views' disparity
        ID3D11Texture2D*        m_depthTex = nullptr;
        ID3D11DepthStencilView* m_dsv = nullptr;
        Tex m_out[2];                                // the new views: composed, then filled
        Tex m_plate[2];                              // what was farthest at each place lately (PSPlate), and the one before
        int   m_plateCur = 0;
        bool  m_plateHave = false, m_plateOn = true;
        float m_motion[4] = {};
        bool  m_markFill = false;
        bool  m_findMotion = true, m_prevHave = false, m_estHave = false;
        Tex   m_prevTmp[kLevels];                    // the levels' colour of the picture before
        Tex   m_lkSum[4], m_est[2], m_estQ;          // the motion finder's sums; the motion (one texel), and the one before; how well it fitted
        int   m_estCur = 0;
        ID3D11Texture2D* m_estRead = nullptr;
        int   m_eyeW = 0, m_eyeH = 0;
        float m_maxShare = 0.25f;   // (a quarter of the eye's width: no pop-out a picture can hold is past it)
        bool  m_haveMaps = false;   // (the disparity maps hold a picture's)
        int   m_spread = 1;
        int   m_base = 0;
        ID3D11Texture2D* m_extDisp = nullptr;
        // (Measured 2026-10-06, quilts: these four together about 1 dB between the eyes, 0.4 beyond them, for 1.7 ms.)
        float m_opt[4] = { 2.0f, 0.02f, 0.01f, 3.0f };
        float m_fineR = 1.0f;       // (3x3: 5x5 measured 0.2 dB more for a millisecond more)
        bool  m_fine = true;        // the answer tried again against the full-size picture (PSFine)

        // (Timing: every so many frames, five timestamps.)
        ID3D11Query* m_disjoint = nullptr; ID3D11Query* m_ts[5] = {};
        bool m_timing = false, m_timePending = false; int m_frame = 0;
        double m_ms[4] = {}; bool m_haveTimes = false;
    };
}
