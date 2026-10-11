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
#include <string>
#include <vector>
#include <unordered_map>

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
        void SetPlate(bool on) { m_plateOn = on; if (!on) m_plateHave = false; }   // (off: not kept at all, nor the motion found for it)
        void ForgetPlate() { m_plateHave = false; }
        void SetMarkFill(bool on) { m_markFill = on; }   // (a test: what the plate filled drawn magenta, what it could not green)
        void SetFarFill(bool on) { m_fillOn = on; }   // (what neither eye sees filled from the far side, level by level; off: the mirrored row)
        // A background painted for the picture in hand (the left eye with near things lifted off and what is
        // behind them painted in): any size, rgb linear, .a that background's disparity in MapSize's texels.
        // What neither eye sees is taken from it first. nullptr: none. It is the caller's to keep alive.
        void SetLayer(ID3D11ShaderResourceView* srv) { m_layer = srv; }
        // The motion between pictures found from the pictures themselves (on, as a rule; off: only what
        // SetMotion gives). ReadMotion: what was found for the last picture -- x, y, per-disparity x, y,
        // how badly it fitted, the share of the picture that counted. It waits for the GPU: for tests.
        void SetFindMotion(bool on) { m_findMotion = on; }
        bool ReadMotion(float out[6]);
        void SetMotion(float x, float y, float perDx, float perDy) { m_motion[0] = x; m_motion[1] = y; m_motion[2] = perDx; m_motion[3] = perDy; }
        // A trial: a disparity from another matcher in place of the one found here. tex: both eyes side by
        // side at MapSize (R16G16B16A16_FLOAT, .r in that map's texels; the left eye's point is at x - d in
        // the right, the right's at x + d in the left). nullptr: its own again.
        // (keepPlate: the picture has moved on since -- a matcher that answers late, picture after picture: the plate
        // goes on. Else the plate was made with another depth for this same picture, and is begun again.)
        void SetExternalDisparity(ID3D11Texture2D* tex, bool keepPlate = false) { m_extDisp = tex; m_haveMaps = false; ++m_extSerial; if (!keepPlate) m_plateHave = false; }
        // Both eyes of src made small (each w x h, side by side, 8-bit sRGB) for a matcher elsewhere to read back.
        ID3D11Texture2D* Shrink(ID3D11ShaderResourceView* src, int eyeW, int eyeH, int w, int h);
        void MapSize(int eyeW, int eyeH, int& w, int& h) const;
        void SetExternalStage(int s) { m_extStage = s; m_haveMaps = false; }
        void SetFuseBias(float b) { m_fuseBias = b; m_haveMaps = false; }
        void SetFuseEdge(bool on) { m_fuseEdge = on; m_haveMaps = false; }
        void SetFuseReach(float t) { m_fuseReach = t; m_haveMaps = false; }
        void SetExternalDilate(int texels) { m_extDilate = texels; m_haveMaps = false; }
        void SetExternalLeftOnly(bool on) { m_extLeftOnly = on; m_haveMaps = false; ++m_extSerial; }
        void SetExternalSnap(int passes) { m_extSnap = passes; m_haveMaps = false; ++m_extSerial; }
        void SetExternalSoft(float z) { m_extSoft = z; }   // (at a step in the other matcher's map, its answer stands)
        // The new views by Gaussian splats: every step-th pixel of each eye thrown forward as a soft spot (0: off --
        // each pixel of the new view looked up in the eyes). sigma: a spot's width in its spacings. seen: the weight
        // under which a pixel counts as reached by nothing.
        void SetGaussian(int step, float sigma = 0.5f, float seen = 0.25f) { m_gauss = step; m_gaussSigma = sigma; m_gaussSeen = seen; }
        // Each pass timed on the graphics card (a test's: it costs a little itself). TakeProfile: the mean ms of each,
        // in the order drawn, since the last asking.
        void SetProfile(bool on) { m_profOn = on; }
        bool TakeProfile(std::vector<std::pair<std::string, double>>& out);
        void SetGaussianScramble(bool on) { m_gaussScramble = on; }
        void SetMotionFinest(int level) { m_lkFinest = level; }
        void SetGaussianSettle(int mode, float margin = 0.004f) { m_gaussSettle = mode; m_settleMargin = margin; m_settleHave = false; }
        void SetGaussianPoints(bool on) { m_gaussPoints = on; }
        void SetGaussianOver(bool on) { m_gaussOver = on; }
        void SetGaussianEdge(int mode) { m_gaussEdge = mode; }   // (0 as the map has it; 1 each spot's side asked of the eyes; 2 and its share of each side: soft)   // (at an outline each spot's side asked of the two eyes: VSGauss)
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
        ID3D11VertexShader*  m_vsFull = nullptr, *m_vsSplat = nullptr, *m_vsGauss = nullptr;
        ID3D11PixelShader*   m_psGauss = nullptr, *m_psGaussResolve = nullptr;
        ID3D11BlendState*    m_blendAdd = nullptr, *m_blendMix = nullptr;
        void Prof(const char* name);
        bool m_profOn = false, m_profFrame = false, m_profPending = false; ID3D11Query* m_profDisjoint = nullptr;
        std::vector<ID3D11Query*> m_profQ; std::vector<const char*> m_profNames; size_t m_profUsed = 0;
        std::vector<std::pair<std::string, double>> m_profSum; int m_profFrames = 0;
        std::unordered_map<ID3D11PixelShader*, const char*> m_psNames;
        Tex   m_accFar;             // (... the layer behind the nearest)
        ID3D11GeometryShader* m_gsGauss = nullptr;
        ID3D11ComputeShader* m_csCells = nullptr;
        ID3D11Buffer*        m_cellBuf = nullptr, *m_cellArgs = nullptr;   // (the cells that have an outline, listed; and how many, as a draw's arguments)
        ID3D11UnorderedAccessView* m_cellUav = nullptr; ID3D11ShaderResourceView* m_cellSrv = nullptr; UINT m_cellCap = 0;
        ID3D11VertexShader*  m_vsCell = nullptr;
        ID3D11PixelShader*   m_psGaussMix = nullptr, *m_psGaussOver = nullptr;
        bool  m_gaussScramble = false;  // (the spots drawn in runs from all over the picture, not in its own order: tried 2026-10-12, no quicker -- 20.3 ms against 19.3)
        int   m_gaussSettle = 0; float m_settleMargin = 0.004f; bool m_settleHave = false;   // (each pixel's own depth settled against the eyes: 0 no, 1 yes, 2 and a spot worth its fit)
        Tex   m_settle; ID3D11VertexShader* m_vsPointS = nullptr; ID3D11PixelShader* m_psSettle = nullptr;
        bool  m_gaussPoints = true;     // (the full mode's spots as single points, spread by a second pass)
        ID3D11VertexShader* m_vsPoint = nullptr; ID3D11PixelShader* m_psPoint = nullptr, *m_psPointResolve = nullptr;
        bool  m_gaussOver = true;   // (the outlines' spots straight onto the picture, one over another; else added up apart and mixed in)
        Tex   m_mix;                // (the outlines' spots laid over the standard picture)
        Tex   m_cover;              // (... and how much of each pixel they cover)
        Tex   m_acc;                // (Gaussian splats: weighted colour and weight, added up)
        int   m_gauss = 0; float m_gaussSigma = 0.5f, m_gaussSeen = 0.25f; int m_gaussEdge = 2;
        ID3D11PixelShader*   m_psDownFirst = nullptr, *m_psDownNext = nullptr, *m_psGrad = nullptr, *m_psMatchFull = nullptr,
                            *m_psMatchRefine = nullptr, *m_psCheck = nullptr, *m_psFill = nullptr, *m_psMedian = nullptr, *m_psSpread = nullptr, *m_psFine = nullptr, *m_psGuide = nullptr, *m_psFuse = nullptr, *m_psDilate = nullptr,
                            *m_psSplat = nullptr, *m_psHole = nullptr, *m_psCompose = nullptr, *m_psUnseen = nullptr, *m_psPlate = nullptr, *m_psFillDown = nullptr, *m_psShrink = nullptr, *m_psRight = nullptr, *m_psRightFill = nullptr, *m_psSnap = nullptr, *m_psLKSum = nullptr, *m_psLKSolve = nullptr;
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
        // The far-side fill (PSFillDown): the new view made smaller level by level, in one texture's mips.
        static constexpr int kFillMax = 9;
        ID3D11Texture2D* m_fillTex = nullptr; ID3D11ShaderResourceView* m_fillAll = nullptr;
        ID3D11RenderTargetView* m_fillRtv[kFillMax] = {}; ID3D11ShaderResourceView* m_fillMip[kFillMax] = {};
        int   m_fillMips = 0, m_fillW = 0, m_fillH = 0;
        bool  m_fillOn = true;
        ID3D11ShaderResourceView* m_layer = nullptr;
        bool  m_findMotion = true, m_prevHave = false, m_estHave = false;
        Tex   m_prevTmp[kLevels];                    // the levels' colour of the picture before
        Tex   m_lkSum[4], m_est[2], m_estQ;          // the motion finder's sums; the motion (one texel), and the one before; how well it fitted
        int   m_estCur = 0;
        ID3D11Texture2D* m_estRead = nullptr;
        int   m_eyeW = 0, m_eyeH = 0;
        float m_maxShare = 0.25f;   // (a quarter of the eye's width: no pop-out a picture can hold is past it)
        Tex   m_extPrep;            // (the other matcher's answer as prepared: kept for the pictures until its next)
        unsigned m_extSerial = 1, m_extPrepSerial = 0;
        int   m_lkFinest = 1;       // (the finest size the motion finder works at: 0 the map's own)
        bool  m_haveMaps = false;   // (the disparity maps hold a picture's)
        int   m_spread = 1;
        int   m_base = 0;
        ID3D11Texture2D* m_extDisp = nullptr;
        int   m_extStage = 4;       // (how it comes in: 0 before the checking, 1 before the edges are drawn in, 2 before the making finer, 3 put right in its detail, 4 the better of it and the look-around's own at each texel)
        float m_fuseBias = 0.5f;    // (4: its own is taken where it costs less than this share of the other's)
        Tex   m_extCopy, m_small;
        bool  m_fuseEdge = true;
        float m_extSoft = 0.01f;    // (with the other matcher's depth, g_opt.z: under 0 an outline is drawn as a contour through the texels, one side or the other; 0 the cheapest of the four; over 0 the surfaces mixed that softly)
        int   m_extSnap = 2;        // (with the other matcher's depth: how many times its ramps are made steps again -- PSSnap; 0: not)
        bool  m_extLeftOnly = false;   // (the other matcher gives the left eye only: the right is made from it -- PSRight)
        int   m_extDilate = 1;      // (with the other matcher's depth: a thing's outline carried this many texels outward -- PSDilate)
        float m_fuseReach = 1.5f;   // (how far, in texels, its own may be from the other matcher's and still be taken; 0: any distance)
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
