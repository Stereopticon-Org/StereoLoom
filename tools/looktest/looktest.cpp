// looktest: SR Loom's look-around (src/LookAround, shaders/Look.hlsl) run on
// the GPU, off the screen, and scored the way tools/splatlab scores its CPU
// version: two views of a Looking Glass quilt are the "two eyes"; every view
// between them and to either side is made and compared with the quilt's own.
//
//   looktest <quilt_qsCxR.png> [refA refB] [reach] [scale] [outdir]
//
// scale: the quilt's views are small (a few hundred pixels across), and the
// matcher works at 1/8 of the eye -- so each view is enlarged by this much
// first (default 8: the matcher then sees the view at its own size, as
// splatlab does; 4 or 2: what it makes of less).
#include <windows.h>
#include <wincodec.h>
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "LookAround.h"
#include "NeuralDepth.h"
#include <string>

using namespace srw;

static bool LoadRGBA(const wchar_t* path, std::vector<uint8_t>& px, UINT& w, UINT& h)
{
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    IWICBitmapDecoder* d = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* c = nullptr;
    bool ok = SUCCEEDED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &d)) &&
              SUCCEEDED(d->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&c)) &&
              SUCCEEDED(c->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(c->GetSize(&w, &h));
    if (ok) { px.resize((size_t)w * h * 4); ok = SUCCEEDED(c->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data())); }
    if (c) c->Release(); if (fr) fr->Release(); if (d) d->Release(); f->Release();
    return ok;
}
static bool SavePNG(const std::wstring& path, const std::vector<uint8_t>& rgba, UINT w, UINT h)
{
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    IWICStream* s = nullptr; IWICBitmapEncoder* e = nullptr; IWICBitmapFrameEncode* fr = nullptr;
    std::vector<uint8_t> bgra(rgba);
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e)) &&
              SUCCEEDED(e->Initialize(s, WICBitmapEncoderNoCache)) && SUCCEEDED(e->CreateNewFrame(&fr, nullptr)) &&
              SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
              SUCCEEDED(fr->WritePixels(h, w * 4, w * 4 * h, bgra.data())) && SUCCEEDED(fr->Commit()) && SUCCEEDED(e->Commit());
    if (fr) fr->Release(); if (e) e->Release(); if (s) s->Release(); f->Release();
    return ok;
}

// A side-by-side picture as it is (a game's frame): no third view to score
// against, so the right eye is made from the LEFT eye alone -- a whole eye
// spacing's move from one view -- and compared with the real right eye. Then
// views from beside the pair are written out to look at.
static ID3D11Texture2D* LoadDsp(ID3D11Device* dev, LookAround& look, const wchar_t* file, int ew, int eh);
static ID3D11ShaderResourceView* LoadLayer(ID3D11Device* dev, LookAround& look, const wchar_t* file, int ew, int eh);
static int RunSbs(const wchar_t* file, const std::wstring& outdir)
{
    std::vector<uint8_t> px; UINT W = 0, H = 0;
    if (!LoadRGBA(file, px, W, H)) { fwprintf(stderr, L"can't load %s\n", file); return 1; }
    const int ew = (int)W / 2, eh = (int)H;
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return 1;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)(ew * 2); td.Height = (UINT)eh; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ px.data(), W * 4, 0 };
    ID3D11Texture2D* src = nullptr; dev->CreateTexture2D(&td, &sd, &src);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr; dev->CreateShaderResourceView(src, &vd, &srv);
    LookAround look;
    if (!srv || !look.Initialize(dev, ctx)) { fwprintf(stderr, L"look-around init failed\n"); return 1; }
    if (const wchar_t* fn = _wgetenv(L"LOOKTEST_FINE")) look.SetFine(_wtoi(fn));
    if (const wchar_t* bs = _wgetenv(L"LOOKTEST_BASE")) look.SetBase(_wtoi(bs));
    if (const wchar_t* op = _wgetenv(L"LOOKTEST_OPT")) { float o4[4] = {}; swscanf_s(op, L"%f,%f,%f,%f", &o4[0], &o4[1], &o4[2], &o4[3]); for (int q = 0; q < 4; ++q) look.SetOpt(q, o4[q]); }
    if (const wchar_t* ds = _wgetenv(L"LOOKTEST_DSP")) look.SetExternalDisparity(LoadDsp(dev, look, ds, ew, eh));
    if (const wchar_t* ly = _wgetenv(L"LOOKTEST_LAYER")) look.SetLayer(LoadLayer(dev, look, ly, ew, eh));
    if (const wchar_t* sn = _wgetenv(L"LOOKTEST_SNAP")) look.SetExternalSnap(_wtoi(sn));
    if (const wchar_t* sf = _wgetenv(L"LOOKTEST_SOFT")) look.SetExternalSoft((float)_wtof(sf));
    if (const wchar_t* xd = _wgetenv(L"LOOKTEST_DILATE")) look.SetExternalDilate(_wtoi(xd));
    if (_wgetenv(L"LOOKTEST_NOPLATE")) look.SetPlate(false);   // (no plate kept: what it costs)
    if (_wgetenv(L"LOOKTEST_MARKFILL")) look.SetMarkFill(true);   // (what filled each unseen pixel, by colour: yellow the painted background, magenta the plate, blue the far side, green the mirrored row)
    if (const wchar_t* es = _wgetenv(L"LOOKTEST_EXTSTAGE")) look.SetExternalStage(_wtoi(es));
    if (const wchar_t* ff = _wgetenv(L"LOOKTEST_FARFILL")) look.SetFarFill(_wtoi(ff) != 0);
    if (const wchar_t* ge = _wgetenv(L"LOOKTEST_GEDGE")) look.SetGaussianEdge(_wtoi(ge) != 0);
    if (const wchar_t* gs = _wgetenv(L"LOOKTEST_GAUSS")) look.SetGaussian(_wtoi(gs), _wgetenv(L"LOOKTEST_GSIGMA") ? (float)_wtof(_wgetenv(L"LOOKTEST_GSIGMA")) : 0.5f, _wgetenv(L"LOOKTEST_GSEEN") ? (float)_wtof(_wgetenv(L"LOOKTEST_GSEEN")) : 0.25f);
    if (const wchar_t* fb = _wgetenv(L"LOOKTEST_FUSE")) look.SetFuseBias((float)_wtof(fb));
    if (const wchar_t* fe = _wgetenv(L"LOOKTEST_FUSEEDGE")) look.SetFuseEdge(_wtoi(fe) != 0);
    if (const wchar_t* fr = _wgetenv(L"LOOKTEST_FUSEREACH")) look.SetFuseReach((float)_wtof(fr));
    if (const wchar_t* xd = _wgetenv(L"LOOKTEST_DILATE")) look.SetExternalDilate(_wtoi(xd));
    // LOOKTEST_NEURAL: the trained matcher as SR Loom runs it (src/NeuralDepth: ONNX Runtime, the model
    // beside the exe), on this picture -- its depth in place of the look-around's own.
    NeuralDepth neural; ID3D11Texture2D* neuralTex = nullptr;
    if (_wgetenv(L"LOOKTEST_NEURAL"))
    {
        wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe; const size_t cut = dir.find_last_of(L"/\134"); if (cut != std::wstring::npos) dir.resize(cut);
        if (!neural.Start(dir, _wgetenv(L"LOOKTEST_MODEL") ? _wgetenv(L"LOOKTEST_MODEL") : L"")) { fwprintf(stderr, L"no neural matcher beside the exe\n"); return 1; }
        if (_wgetenv(L"LOOKTEST_LEFTONLY")) { neural.SetLeftOnly(true); look.SetExternalLeftOnly(true); }
        for (int k = 0; k < 1200 && !neural.Ready() && !neural.Failed(); ++k) Sleep(100);
        if (!neural.Ready()) { fwprintf(stderr, L"the model did not load\n"); return 1; }
        int mw = 0, mh = 0; look.MapSize(ew, eh, mw, mh);
        const float aspect = _wgetenv(L"LOOKTEST_ASPECT") ? (float)_wtof(_wgetenv(L"LOOKTEST_ASPECT")) : 16.0f / 9.0f;
        for (int run = 0; run < 2; ++run)   // (twice: the first has the model's warming up in it)
        {
            neural.Submit(px.data(), (int)W * 4, ew, eh, aspect, 736, mw, mh, 1u);
            std::vector<uint16_t> res; int rw = 0, rh = 0; unsigned rs = 0; double ms = 0;
            while (!neural.Take(res, rw, rh, rs, ms)) { if (neural.Failed()) { fwprintf(stderr, L"the model failed\n"); return 1; } Sleep(20); }
            wprintf(L"neural matcher: both eyes in %.0f ms, map %dx%d\n", ms, rw, rh);
            if (run == 0) continue;
            D3D11_TEXTURE2D_DESC nd{};
            nd.Width = (UINT)(rw * 2); nd.Height = (UINT)rh; nd.MipLevels = 1; nd.ArraySize = 1; nd.SampleDesc.Count = 1;
            nd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{ res.data(), (UINT)(rw * 2 * 8), 0 };
            dev->CreateTexture2D(&nd, &init, &neuralTex);
            look.SetExternalDisparity(neuralTex);
            // LOOKTEST_PAINT: ... and the painted background for it (the inpainting model beside the exe).
            if (_wgetenv(L"LOOKTEST_PAINT"))
            {
                if (!neural.RequestLayer(1u, mw)) { fwprintf(stderr, L"no painter beside the exe\n"); return 1; }
                std::vector<float> lay; int ln = 0; unsigned ls = 0; double lms = 0;
                for (int k = 0; k < 3000 && !neural.TakeLayer(lay, ln, ls, lms); ++k) { if (!neural.PainterThere()) break; Sleep(20); }
                if (ln <= 0) { fwprintf(stderr, L"the painter gave nothing\n"); return 1; }
                wprintf(L"background painted in %.0f ms (%dx%d)\n", lms, ln, ln);
                // (... and twice more, for what it takes once it is loaded.)
                for (int again = 0; again < 2; ++again)
                {
                    if (!neural.RequestLayer(1u, mw)) break;
                    std::vector<float> l2; int n2 = 0; unsigned s2 = 0; double m2 = 0;
                    for (int k = 0; k < 3000 && !neural.TakeLayer(l2, n2, s2, m2); ++k) Sleep(5);
                    wprintf(L"   again: %.0f ms\n", m2);
                }
                D3D11_TEXTURE2D_DESC ld{};
                ld.Width = (UINT)ln; ld.Height = (UINT)ln; ld.MipLevels = 1; ld.ArraySize = 1; ld.SampleDesc.Count = 1;
                ld.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; ld.Usage = D3D11_USAGE_DEFAULT; ld.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA linit{ lay.data(), (UINT)(ln * 16), 0 };
                ID3D11Texture2D* lt = nullptr; ID3D11ShaderResourceView* lsrv = nullptr;
                if (SUCCEEDED(dev->CreateTexture2D(&ld, &linit, &lt))) { dev->CreateShaderResourceView(lt, nullptr, &lsrv); lt->Release(); }
                look.SetLayer(lsrv);
            }
        }
    }
    ID3D11Texture2D* staging = nullptr;
    auto read = [&](std::vector<uint8_t>& out) {
        D3D11_TEXTURE2D_DESC od{}; look.OutputTexture()->GetDesc(&od);
        if (!staging) { od.Usage = D3D11_USAGE_STAGING; od.BindFlags = 0; od.CPUAccessFlags = D3D11_CPU_ACCESS_READ; od.MiscFlags = 0; od.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dev->CreateTexture2D(&od, nullptr, &staging); }
        ctx->CopyResource(staging, look.OutputTexture());
        D3D11_MAPPED_SUBRESOURCE m{}; ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
        out.resize((size_t)ew * 2 * eh * 4);
        for (int y = 0; y < eh; ++y) memcpy(&out[(size_t)y * ew * 2 * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)ew * 2 * 4);
        ctx->Unmap(staging, 0);
    };
    // Right from left alone, and left from right alone.
    double ps[2] = {}, bad[2] = {};
    for (int from = 0; from < 2; ++from)
    {
        LookAround::View v; v.only = from;
        if (from == 0) { v.tL = 1.0f; v.tR = 1.0f; } else { v.tL = 0.0f; v.tR = 0.0f; }
        std::vector<uint8_t> o; look.Render(srv, ew, eh, v); read(o);
        // (The made view is in the left half; the real one is the other eye's half of the picture.)
        const int margin = ew / 12; double s = 0; size_t n = 0, b = 0, np = 0;
        for (int y = margin; y < eh - margin; ++y) for (int x = margin; x < ew - margin; ++x, ++np)
        {
            const uint8_t* a = &o[((size_t)y * ew * 2 + x) * 4];
            const uint8_t* t = &px[((size_t)y * W + (from == 0 ? ew : 0) + x) * 4];
            int worst = 0;
            for (int k = 0; k < 3; ++k) { const int d = (int)a[k] - (int)t[k]; s += (double)d * d; ++n; worst = (std::max)(worst, std::abs(d)); }
            b += worst > 40;
        }
        ps[from] = 10.0 * std::log10(255.0 * 255.0 / (std::max)(s / (std::max)(n, (size_t)1), 1e-9));
        bad[from] = 100.0 * b / (std::max)(np, (size_t)1);
    }
    wprintf(L"side-by-side %dx%d eyes; each eye made from the other alone: right from left %.2f dB, %.2f%% of pixels off by > 40 | left from right %.2f dB, %.2f%%\n",
            ew, eh, ps[0], bad[0], ps[1], bad[1]);
    {
        double sum[4] = {}; int n = 0;
        LookAround::View v; v.tL = 0.4f; v.tR = 1.4f;
        for (int f = 0; f < 200; ++f)
        {
            look.Render(srv, ew, eh, v); ctx->Flush();
            double ms[4]; if (look.TakeTimes(ms)) { for (int k = 0; k < 4; ++k) sum[k] += ms[k]; ++n; }
            if (f % 8 == 7) Sleep(5);
        }
        if (n) wprintf(L"GPU ms a frame (mean of %d): levels %.2f, matching %.2f, geometry %.2f, new views %.2f | total %.2f\n",
                       n, sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, (sum[0] + sum[1] + sum[2] + sum[3]) / n);
    }
    if (!outdir.empty())
    {
        // The left eye's disparity as a grey picture (near is light), at its own size.
        LookAround::View v0; look.Render(srv, ew, eh, v0);
        D3D11_TEXTURE2D_DESC dd{}; look.DisparityTexture()->GetDesc(&dd);
        D3D11_TEXTURE2D_DESC sd2 = dd; sd2.Usage = D3D11_USAGE_STAGING; sd2.BindFlags = 0; sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* st = nullptr; dev->CreateTexture2D(&sd2, nullptr, &st);
        ctx->CopyResource(st, look.DisparityTexture());
        D3D11_MAPPED_SUBRESOURCE m{}; ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
        const int dw = (int)dd.Width / 2, dh = (int)dd.Height;
        auto half = [](uint16_t hh) { const uint32_t s = (hh >> 15) & 1, e = (hh >> 10) & 31, mm = hh & 1023; float v = e == 0 ? std::ldexp((float)mm, -24) : std::ldexp((float)(mm | 1024), (int)e - 25); return s ? -v : v; };
        std::vector<float> dv((size_t)dw * dh); float lo = 1e9f, hi = -1e9f;
        for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x)
        {
            const uint16_t* p = (const uint16_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch) + (size_t)x * 4;
            const float v = half(p[0]); dv[(size_t)y * dw + x] = v; lo = (std::min)(lo, v); hi = (std::max)(hi, v);
        }
        ctx->Unmap(st, 0); st->Release();
        std::vector<uint8_t> g((size_t)dw * dh * 4);
        for (int k = 0; k < dw * dh; ++k) { const uint8_t c = (uint8_t)(255.0f * (dv[k] - lo) / (std::max)(hi - lo, 1e-3f)); g[k * 4] = g[k * 4 + 1] = g[k * 4 + 2] = c; g[k * 4 + 3] = 255; }
        SavePNG(outdir + L"\\look_disparity.png", g, (UINT)dw, (UINT)dh);
        wprintf(L"written: look_disparity.png (%dx%d, disparity %.1f .. %.1f texels)\n", dw, dh, lo, hi);
    }
    if (!outdir.empty())
    {
        // To look at, one under another (both eyes each): the head an eye spacing
        // to the left; half a spacing to the right and up; nearer the screen.
        LookAround::View vs[3];
        vs[0].tL = -1.0f; vs[0].tR = 0.0f;
        vs[1].tL = 0.5f; vs[1].tR = 1.5f; vs[1].ty = 0.5f;
        vs[2].rho = 1.5f;
        std::vector<uint8_t> sheet((size_t)ew * 2 * eh * 3 * 4), o;
        for (int r = 0; r < 3; ++r) { look.Render(srv, ew, eh, vs[r]); read(o); memcpy(&sheet[(size_t)ew * 2 * eh * 4 * r], o.data(), o.size()); }
        SavePNG(outdir + L"\\look_sbs.png", sheet, (UINT)(ew * 2), (UINT)(eh * 3));
        wprintf(L"written: look_sbs.png (both eyes; rows: head a spacing left | half a spacing right and up | nearer the screen)\n");
    }
    look.Shutdown();
    return 0;
}

// A disparity from another matcher (LOOKTEST_DSP: 'DSP1', width, height, then
// the left and the right eye's disparity in the picture's own pixels, float
// rows) made into the texture LookAround::SetExternalDisparity takes: each
// texel of the map the mean of the pixels it covers, in the map's texels.
static ID3D11Texture2D* LoadDsp(ID3D11Device* dev, LookAround& look, const wchar_t* file, int ew, int eh)
{
    FILE* f = nullptr; _wfopen_s(&f, file, L"rb");
    if (!f) return nullptr;
    char tag[4]; int w = 0, h = 0;
    if (fread(tag, 1, 4, f) != 4 || fread(&w, 4, 1, f) != 1 || fread(&h, 4, 1, f) != 1 || w != ew || h != eh) { fclose(f); fwprintf(stderr, L"the disparity file is not this picture's size\n"); return nullptr; }
    std::vector<float> d((size_t)w * h * 2);
    fread(d.data(), sizeof(float), d.size(), f); fclose(f);
    int mw = 0, mh = 0; look.MapSize(ew, eh, mw, mh);
    const int kx = ew / mw, ky = eh / mh;
    // (A test: the disparity scaled and shifted, LOOKTEST_DSPGAIN / LOOKTEST_DSPBIAS (pixels).)
    const float gain = _wgetenv(L"LOOKTEST_DSPGAIN") ? (float)_wtof(_wgetenv(L"LOOKTEST_DSPGAIN")) : 1.0f, bias = _wgetenv(L"LOOKTEST_DSPBIAS") ? (float)_wtof(_wgetenv(L"LOOKTEST_DSPBIAS")) : 0.0f;
    auto toHalf = [](float v) -> uint16_t {
        uint32_t x; memcpy(&x, &v, 4);
        const uint32_t sign = (x >> 16) & 0x8000; int e = (int)((x >> 23) & 0xff) - 127 + 15; uint32_t m = x & 0x7fffff;
        if (e <= 0) { if (e < -10) return (uint16_t)sign; m = (m | 0x800000) >> (1 - e); return (uint16_t)(sign | (m >> 13)); }
        if (e >= 31) return (uint16_t)(sign | 0x7c00);
        return (uint16_t)(sign | (e << 10) | (m >> 13));
    };
    std::vector<uint16_t> px((size_t)mw * 2 * mh * 4, 0);
    for (int eye = 0; eye < 2; ++eye)
        for (int y = 0; y < mh; ++y) for (int x = 0; x < mw; ++x)
        {
            double s = 0;
            for (int j = 0; j < ky; ++j) for (int i = 0; i < kx; ++i) s += d[(size_t)eye * w * h + (size_t)(y * ky + j) * w + (x * kx + i)];
            px[((size_t)y * mw * 2 + eye * mw + x) * 4] = toHalf(((float)(s / (kx * ky)) * gain + bias) / (float)kx);
            px[((size_t)y * mw * 2 + eye * mw + x) * 4 + 1] = toHalf(1.0f);   // (.g: sure of it)
        }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)(mw * 2); td.Height = (UINT)mh; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ px.data(), (UINT)(mw * 2 * 8), 0 };
    ID3D11Texture2D* t = nullptr; dev->CreateTexture2D(&td, &sd, &t);
    if (t) wprintf(L"disparity from %s, as a %dx%d map\n", file, mw, mh);
    return t;
}

// A painted background (LOOKTEST_LAYER: 'LYR1', width, height, then rows of four floats -- red, green, blue
// 0..255 as stored, and the disparity as a share of the width) as LookAround::SetLayer takes it.
static ID3D11ShaderResourceView* LoadLayer(ID3D11Device* dev, LookAround& look, const wchar_t* file, int ew, int eh)
{
    FILE* f = nullptr; _wfopen_s(&f, file, L"rb");
    if (!f) return nullptr;
    char tag[4]; int w = 0, h = 0;
    if (fread(tag, 1, 4, f) != 4 || fread(&w, 4, 1, f) != 1 || fread(&h, 4, 1, f) != 1 || w < 8 || h < 8) { fclose(f); return nullptr; }
    std::vector<float> v((size_t)w * h * 4);
    fread(v.data(), sizeof(float), v.size(), f); fclose(f);
    int mw = 0, mh = 0; look.MapSize(ew, eh, mw, mh);
    for (size_t k = 0; k < (size_t)w * h; ++k)
    {
        for (int c = 0; c < 3; ++c) { const float s = v[k * 4 + c] / 255.0f; v[k * 4 + c] = s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f); }
        v[k * 4 + 3] *= (float)mw;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ v.data(), (UINT)(w * 16), 0 };
    ID3D11Texture2D* t = nullptr; ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &t))) { dev->CreateShaderResourceView(t, nullptr, &srv); t->Release(); }
    if (srv) wprintf(L"painted background from %s (%dx%d)\n", file, w, h);
    return srv;
}

// Against a measured truth: a Middlebury 2014 scene (im0.png, im1.png and
// disp0.pfm, the left eye's true disparity in pixels). The disparity found for
// the left eye is compared with it: the share of the picture out by more than
// 2, 4 and 8 pixels, and the mean error -- figures other methods publish too.
static int RunMb(const std::wstring& dir)
{
    std::vector<uint8_t> a, b; UINT W = 0, H = 0, W2 = 0, H2 = 0;
    if (!LoadRGBA((dir + L"/im0.png").c_str(), a, W, H) || !LoadRGBA((dir + L"/im1.png").c_str(), b, W2, H2) || W != W2 || H != H2) { fwprintf(stderr, L"can't load the scene\n"); return 1; }
    std::vector<float> gt((size_t)W * H);
    {
        FILE* f = nullptr; _wfopen_s(&f, (dir + L"/disp0.pfm").c_str(), L"rb");
        if (!f) { fwprintf(stderr, L"no disp0.pfm\n"); return 1; }
        char tag[8] = {}; int pw = 0, ph = 0; float sc = 0;
        if (fscanf_s(f, "%7s %d %d %f", tag, 8, &pw, &ph, &sc) != 4 || pw != (int)W || ph != (int)H) { fclose(f); fwprintf(stderr, L"bad pfm\n"); return 1; }
        fgetc(f);
        for (int y = (int)H - 1; y >= 0; --y) fread(&gt[(size_t)y * W], sizeof(float), W, f);   // (bottom row first)
        fclose(f);
    }
    const int ew = (int)W, eh = (int)H;
    std::vector<uint8_t> sbs((size_t)ew * 2 * eh * 4);
    for (int y = 0; y < eh; ++y)
    {
        memcpy(&sbs[(size_t)y * ew * 2 * 4], &a[(size_t)y * ew * 4], (size_t)ew * 4);
        memcpy(&sbs[((size_t)y * ew * 2 + ew) * 4], &b[(size_t)y * ew * 4], (size_t)ew * 4);
    }
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return 1;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)(ew * 2); td.Height = (UINT)eh; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ sbs.data(), (UINT)(ew * 2 * 4), 0 };
    ID3D11Texture2D* src = nullptr; dev->CreateTexture2D(&td, &sd, &src);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr; dev->CreateShaderResourceView(src, &vd, &srv);
    LookAround look;
    if (!srv || !look.Initialize(dev, ctx)) { fwprintf(stderr, L"look-around init failed\n"); return 1; }
    if (const wchar_t* bs = _wgetenv(L"LOOKTEST_BASE")) look.SetBase(_wtoi(bs));
    if (const wchar_t* ds = _wgetenv(L"LOOKTEST_DSP")) look.SetExternalDisparity(LoadDsp(dev, look, ds, ew, eh));
    if (const wchar_t* es = _wgetenv(L"LOOKTEST_EXTSTAGE")) look.SetExternalStage(_wtoi(es));
    if (const wchar_t* fb = _wgetenv(L"LOOKTEST_FUSE")) look.SetFuseBias((float)_wtof(fb));
    if (const wchar_t* fe = _wgetenv(L"LOOKTEST_FUSEEDGE")) look.SetFuseEdge(_wtoi(fe) != 0);
    if (const wchar_t* fr = _wgetenv(L"LOOKTEST_FUSEREACH")) look.SetFuseReach((float)_wtof(fr));
    if (const wchar_t* xd = _wgetenv(L"LOOKTEST_DILATE")) look.SetExternalDilate(_wtoi(xd));
    if (const wchar_t* dp = _wgetenv(L"LOOKTEST_DUMPPAIR")) { SavePNG(std::wstring(dp) + L"/left.png", a, (UINT)ew, (UINT)eh); SavePNG(std::wstring(dp) + L"/right.png", b, (UINT)ew, (UINT)eh); }
    LookAround::View v0; look.Render(srv, ew, eh, v0);
    D3D11_TEXTURE2D_DESC dd{}; look.DisparityTexture()->GetDesc(&dd);
    D3D11_TEXTURE2D_DESC sd2 = dd; sd2.Usage = D3D11_USAGE_STAGING; sd2.BindFlags = 0; sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr; dev->CreateTexture2D(&sd2, nullptr, &st);
    ctx->CopyResource(st, look.DisparityTexture());
    D3D11_MAPPED_SUBRESOURCE m{}; ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
    const int dw = (int)dd.Width / 2, dh = (int)dd.Height;
    auto half = [](uint16_t hh) { const uint32_t s = (hh >> 15) & 1, e = (hh >> 10) & 31, mm = hh & 1023; float v = e == 0 ? std::ldexp((float)mm, -24) : std::ldexp((float)(mm | 1024), (int)e - 25); return s ? -v : v; };
    const float sx = (float)ew / (float)dw, sy = (float)eh / (float)dh;
    size_t n = 0, b2 = 0, b4 = 0, b8 = 0; double sum = 0;
    // (... and the same at outlines alone: texels within two of a step in the true disparity.)
    size_t ne = 0, e4 = 0, e8 = 0;
    auto gtAt = [&](int x, int y) { x = (std::max)(0, (std::min)(dw - 1, x)); y = (std::max)(0, (std::min)(dh - 1, y)); return gt[(size_t)(std::min)(eh - 1, (int)(((float)y + 0.5f) * sy)) * W + (std::min)(ew - 1, (int)(((float)x + 0.5f) * sx))]; };
    for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x)
    {
        const float g = gt[(size_t)(std::min)(eh - 1, (int)(((float)y + 0.5f) * sy)) * W + (std::min)(ew - 1, (int)(((float)x + 0.5f) * sx))];
        if (!(g > 0.0f) || !std::isfinite(g)) continue;
        const uint16_t* p = (const uint16_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch) + (size_t)x * 4;
        const float e = std::fabs(half(p[0]) * sx - g);
        ++n; sum += e; b2 += e > 2.0f; b4 += e > 4.0f; b8 += e > 8.0f;
        bool edge = false;
        for (int o = 1; o <= 2 && !edge; ++o)
        {
            const float nb[4] = { gtAt(x - o, y), gtAt(x + o, y), gtAt(x, y - o), gtAt(x, y + o) };
            for (int q = 0; q < 4; ++q) if (std::isfinite(nb[q]) && nb[q] > 0.0f && std::fabs(nb[q] - g) > 1.5f * sx) edge = true;
        }
        if (edge) { ++ne; e4 += e > 4.0f; e8 += e > 8.0f; }
    }
    ctx->Unmap(st, 0); st->Release();
    const std::wstring name = dir.substr(dir.find_last_of(L"/") + 1);
    wprintf(L"%-20s %dx%d, map %dx%d: out by > 2 px %5.1f%% | > 4 px %5.1f%% | > 8 px %5.1f%% | mean error %.2f px\n", name.c_str(), ew, eh, dw, dh,
            100.0 * b2 / (std::max)(n, (size_t)1), 100.0 * b4 / (std::max)(n, (size_t)1), 100.0 * b8 / (std::max)(n, (size_t)1), sum / (std::max)(n, (size_t)1));
    wprintf(L"%-20s at outlines (%.1f%% of the picture): out by > 4 px %5.1f%% | > 8 px %5.1f%%\n", name.c_str(), 100.0 * ne / (std::max)(n, (size_t)1), 100.0 * e4 / (std::max)(ne, (size_t)1), 100.0 * e8 / (std::max)(ne, (size_t)1));
    look.Shutdown();
    return 0;
}

// The live path as SR Loom's loop runs it, without SR Loom: the picture made small and read back without
// waiting, handed to the trained matcher, each answer and each painted background taken as it comes, a
// new view drawn every loop -- for some seconds, with the head swaying. Prints how many answers and
// backgrounds came, how long each took, and how bright the output is (a black or broken output shows as
// a brightness far from the picture's own).
static int RunLive(const wchar_t* file)
{
    std::vector<uint8_t> px; UINT W = 0, H = 0;
    if (!LoadRGBA(file, px, W, H)) { fwprintf(stderr, L"can't load %s\n", file); return 1; }
    const int ew = (int)W / 2, eh = (int)H;
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return 1;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)(ew * 2); td.Height = (UINT)eh; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ px.data(), W * 4, 0 };
    ID3D11Texture2D* src = nullptr; dev->CreateTexture2D(&td, &sd, &src);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr; dev->CreateShaderResourceView(src, &vd, &srv);
    LookAround look;
    if (!srv || !look.Initialize(dev, ctx)) { fwprintf(stderr, L"look-around init failed\n"); return 1; }
    look.SetPlate(false); look.SetExternalStage(2); look.SetExternalLeftOnly(true);
    NeuralDepth neural;
    wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe; const size_t cut = dir.find_last_of(L"/\\"); if (cut != std::wstring::npos) dir.resize(cut);
    if (!neural.Start(dir, _wgetenv(L"LOOKTEST_MODEL") ? _wgetenv(L"LOOKTEST_MODEL") : L"")) { fwprintf(stderr, L"no neural matcher beside the exe\n"); return 1; }
    neural.SetLeftOnly(true); neural.SetAutoPaint(_wgetenv(L"LOOKTEST_NOPAINT") == nullptr); if (_wgetenv(L"LOOKTEST_PAINTGAP")) neural.SetPaintGap(_wtoi(_wgetenv(L"LOOKTEST_PAINTGAP")));
    for (int k = 0; k < 6000 && !neural.Ready() && !neural.Failed(); ++k) Sleep(100);
    if (!neural.Ready()) { fwprintf(stderr, L"the model did not load\n"); return 1; }
    wprintf(L"runtime: %s\n", neural.OnTensorRT() ? L"TensorRT" : L"DirectML");
    int mw = 0, mh = 0; look.MapSize(ew, eh, mw, mh);
    const float aspect = 16.0f / 9.0f;
    ID3D11Texture2D* stage = nullptr, *outStage = nullptr, *depthTex = nullptr; ID3D11ShaderResourceView* layerSrv = nullptr;
    bool copying = false, active = false; int nw = 0, nh = 0; unsigned serial = 1;
    int answers = 0, layers = 0; double ansMs = 0, layMs = 0;
    const ULONGLONG t0 = GetTickCount64(); ULONGLONG lastPrint = t0; int loops = 0;
    const int seconds = _wgetenv(L"LOOKTEST_SECONDS") ? _wtoi(_wgetenv(L"LOOKTEST_SECONDS")) : 8;
    const bool trace = _wgetenv(L"LOOKTEST_TRACE") != nullptr;   // (each step's time printed)
    while (GetTickCount64() - t0 < (ULONGLONG)seconds * 1000)
    {
        ++loops; ++serial;                                   // (every loop a "new picture": a video)
        if (!copying && neural.Remaining() <= 14.0)
        {
            neural.NetSize(aspect, 736, nw, nh);
            ID3D11Texture2D* shrunk = look.Shrink(srv, ew, eh, nw, nh);
            if (shrunk && !stage) { D3D11_TEXTURE2D_DESC st{}; shrunk->GetDesc(&st); st.Usage = D3D11_USAGE_STAGING; st.BindFlags = 0; st.CPUAccessFlags = D3D11_CPU_ACCESS_READ; st.MiscFlags = 0; dev->CreateTexture2D(&st, nullptr, &stage); }
            if (shrunk && stage) { ctx->CopyResource(stage, shrunk); ctx->Flush(); copying = true; if (trace) wprintf(L"  %5llu ms: copy begun\n", GetTickCount64() - t0); }
        }
        if (copying)
        {
            D3D11_MAPPED_SUBRESOURCE m{};
            const HRESULT hr = ctx->Map(stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (SUCCEEDED(hr)) { const bool took = neural.Submit((const uint8_t*)m.pData, (int)m.RowPitch, nw, nh, aspect, 736, mw, mh, serial); ctx->Unmap(stage, 0); copying = false; if (trace) wprintf(L"  %5llu ms: read and handed over (%d)\n", GetTickCount64() - t0, took ? 1 : 0); }
            else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) copying = false;
        }
        std::vector<uint16_t> res; int rw = 0, rh = 0; unsigned rs = 0; double rms = 0;
        if (neural.Take(res, rw, rh, rs, rms) && rw == mw && rh == mh)
        {
            if (depthTex) depthTex->Release(); depthTex = nullptr;
            D3D11_TEXTURE2D_DESC nd{};
            nd.Width = (UINT)(rw * 2); nd.Height = (UINT)rh; nd.MipLevels = 1; nd.ArraySize = 1; nd.SampleDesc.Count = 1;
            nd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{ res.data(), (UINT)(rw * 2 * 8), 0 };
            if (SUCCEEDED(dev->CreateTexture2D(&nd, &init, &depthTex))) { look.SetExternalDisparity(depthTex, active); active = true; ++answers; ansMs += rms; if (trace) wprintf(L"  %5llu ms: answer (%.0f ms in the thread)\n", GetTickCount64() - t0, rms); }
        }
        std::vector<float> lay; int ln = 0; unsigned ls = 0; double lms = 0;
        if (neural.TakeLayer(lay, ln, ls, lms))
        {
            D3D11_TEXTURE2D_DESC ld{};
            ld.Width = (UINT)ln; ld.Height = (UINT)ln; ld.MipLevels = 1; ld.ArraySize = 1; ld.SampleDesc.Count = 1;
            ld.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; ld.Usage = D3D11_USAGE_DEFAULT; ld.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{ lay.data(), (UINT)(ln * 16), 0 };
            ID3D11Texture2D* lt = nullptr;
            if (SUCCEEDED(dev->CreateTexture2D(&ld, &init, &lt)))
            {
                if (layerSrv) layerSrv->Release(); layerSrv = nullptr;
                dev->CreateShaderResourceView(lt, nullptr, &layerSrv); lt->Release();
                look.SetLayer(layerSrv); ++layers; layMs += lms;
            }
        }
        LookAround::View v; const float sway = 0.4f * std::sin((float)(GetTickCount64() - t0) * 0.002f);
        v.tL = sway; v.tR = 1.0f + sway;
        look.Render(srv, ew, eh, v, true);
        ctx->Flush();
        // (Paced as a display paces SR Loom -- LOOKTEST_HZ, 160 unless said; 0: as fast as it goes. Unpaced, the
        // card's queue is always full and the picture read back waits behind it: no measure of the real thing.)
        {
            static const double hz = _wgetenv(L"LOOKTEST_HZ") ? _wtof(_wgetenv(L"LOOKTEST_HZ")) : 160.0;
            static LARGE_INTEGER qf{}, next{};
            if (hz > 0.0)
            {
                if (!qf.QuadPart) { QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&next); }
                next.QuadPart += (LONGLONG)((double)qf.QuadPart / hz);
                LARGE_INTEGER q{}; QueryPerformanceCounter(&q);
                if (q.QuadPart > next.QuadPart) next = q;   // (late: no catching up)
                while (q.QuadPart < next.QuadPart) { YieldProcessor(); QueryPerformanceCounter(&q); }
            }
        }
        const ULONGLONG now = GetTickCount64();
        if (now - lastPrint >= 2000)
        {
            // (The output's mean brightness, read back -- waiting for it, this once in a while.)
            D3D11_TEXTURE2D_DESC od{}; look.OutputTexture()->GetDesc(&od);
            if (!outStage) { od.Usage = D3D11_USAGE_STAGING; od.BindFlags = 0; od.CPUAccessFlags = D3D11_CPU_ACCESS_READ; od.MiscFlags = 0; od.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dev->CreateTexture2D(&od, nullptr, &outStage); }
            ctx->CopyResource(outStage, look.OutputTexture());
            D3D11_MAPPED_SUBRESOURCE m{}; double sum = 0; size_t n = 0, black = 0;
            if (SUCCEEDED(ctx->Map(outStage, 0, D3D11_MAP_READ, 0, &m)))
            {
                for (int y = 0; y < eh; y += 8) for (int x = 0; x < ew * 2; x += 8)
                {
                    const uint8_t* p = (const uint8_t*)m.pData + (size_t)y * m.RowPitch + (size_t)x * 4;
                    const int l = p[0] + p[1] + p[2]; sum += l / 3.0; ++n; black += l == 0;
                }
                ctx->Unmap(outStage, 0);
            }
            double in = 0; size_t ni = 0;
            for (size_t k = 0; k < (size_t)W * H; k += 64) { in += (px[k * 4] + px[k * 4 + 1] + px[k * 4 + 2]) / 3.0; ++ni; }
            wprintf(L"%4.1f s: %d loops, %d depth answers (%.0f ms each), %d backgrounds (%.0f ms each); output brightness %.1f (picture %.1f), wholly black pixels %.1f%%\n",
                    (now - t0) / 1000.0, loops, answers, answers ? ansMs / answers : 0.0, layers, layers ? layMs / layers : 0.0, n ? sum / n : 0.0, ni ? in / ni : 0.0, n ? 100.0 * black / n : 0.0);
            { double tp = 0, tr = 0, ta = 0; neural.Times(tp, tr, ta); wprintf(L"        the matcher's thread: inputs %.1f ms, the model %.1f ms, all %.1f ms\n", tp, tr, ta); }
            lastPrint = now; loops = 0; answers = 0; layers = 0; ansMs = 0; layMs = 0;
        }
    }
    neural.Shutdown(); look.Shutdown();
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { fwprintf(stderr, L"usage: looktest <quilt_qsCxR.png> [refA refB] [reach] [scale] [outdir]\n"); return 1; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (argc >= 3 && !wcscmp(argv[1], L"sbs")) return RunSbs(argv[2], argc > 3 ? argv[3] : L"");
    if (argc >= 3 && !wcscmp(argv[1], L"live")) return RunLive(argv[2]);
    if (argc >= 3 && !wcscmp(argv[1], L"mb")) return RunMb(argv[2]);
    std::wstring path = argv[1];
    int C = 8, R = 6;
    { const size_t q = path.rfind(L"qs"); if (q != std::wstring::npos) swscanf_s(path.c_str() + q, L"qs%dx%d", &C, &R); }
    std::vector<uint8_t> px; UINT qw = 0, qh = 0;
    if (!LoadRGBA(path.c_str(), px, qw, qh)) { fwprintf(stderr, L"can't load %s\n", path.c_str()); return 1; }
    const int N = C * R, vw = (int)qw / C, vh = (int)qh / R;
    const int ia = argc > 3 ? _wtoi(argv[2]) : N / 2 - 2, ib = argc > 3 ? _wtoi(argv[3]) : N / 2 + 2;
    const int step = ib - ia;
    const int reach = argc > 4 ? _wtoi(argv[4]) : step;
    const int scale = argc > 5 ? (std::max)(1, _wtoi(argv[5])) : 8;
    const std::wstring outdir = argc > 6 ? argv[6] : L"";
    if (ia < 0 || ib >= N || step <= 0) { fwprintf(stderr, L"bad reference views\n"); return 1; }
    const int ew = vw * scale, eh = vh * scale;
    // A view of the quilt, enlarged (bilinear), as RGBA bytes. (View 0 is the
    // bottom-left tile; they run left to right, then up.)
    auto view = [&](int v, std::vector<uint8_t>& out) {
        out.resize((size_t)ew * eh * 4);
        const int col = v % C, row = R - 1 - v / C;
        for (int y = 0; y < eh; ++y) for (int x = 0; x < ew; ++x)
        {
            const float sx = (std::max)(0.0f, (std::min)((float)vw - 1.001f, ((float)x + 0.5f) / scale - 0.5f));
            const float sy = (std::max)(0.0f, (std::min)((float)vh - 1.001f, ((float)y + 0.5f) / scale - 0.5f));
            const int x0 = (int)sx, y0 = (int)sy; const float fx = sx - x0, fy = sy - y0;
            auto at = [&](int xx, int yy) { return &px[((size_t)(row * vh + yy) * qw + col * vw + xx) * 4]; };
            for (int k = 0; k < 3; ++k)
                out[((size_t)y * ew + x) * 4 + k] = (uint8_t)((at(x0, y0)[k] * (1 - fx) + at(x0 + 1, y0)[k] * fx) * (1 - fy) + (at(x0, y0 + 1)[k] * (1 - fx) + at(x0 + 1, y0 + 1)[k] * fx) * fy + 0.5f);
            out[((size_t)y * ew + x) * 4 + 3] = 255;
        }
    };

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
    { fwprintf(stderr, L"no D3D11 device\n"); return 1; }

    // The two eyes side by side, as the converter hands them on.
    std::vector<uint8_t> A, B, sbs((size_t)ew * 2 * eh * 4);
    view(ia, A); view(ib, B);
    for (int y = 0; y < eh; ++y)
    {
        memcpy(&sbs[(size_t)y * ew * 2 * 4], &A[(size_t)y * ew * 4], (size_t)ew * 4);
        memcpy(&sbs[((size_t)y * ew * 2 + ew) * 4], &B[(size_t)y * ew * 4], (size_t)ew * 4);
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)(ew * 2); td.Height = (UINT)eh; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ sbs.data(), (UINT)(ew * 2 * 4), 0 };
    ID3D11Texture2D* src = nullptr; dev->CreateTexture2D(&td, &sd, &src);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr; dev->CreateShaderResourceView(src, &vd, &srv);
    if (!srv) { fwprintf(stderr, L"no source texture\n"); return 1; }

    LookAround look;
    if (!look.Initialize(dev, ctx)) { fwprintf(stderr, L"look-around init failed\n"); return 1; }
    if (const wchar_t* m = _wgetenv(L"LOOKTEST_MAXDISP")) look.SetMaxDisparity((float)_wtof(m));
    if (const wchar_t* sp = _wgetenv(L"LOOKTEST_SPREAD")) look.SetSpread(_wtoi(sp));
    if (const wchar_t* fn = _wgetenv(L"LOOKTEST_FINE")) look.SetFine(_wtoi(fn));
    if (const wchar_t* bs = _wgetenv(L"LOOKTEST_BASE")) look.SetBase(_wtoi(bs));
    // LOOKTEST_DUMPPAIR=dir: the two eyes written out (left.png, right.png) for another matcher to work on;
    // LOOKTEST_DSP: its answer taken in place of the look-around's own (LOOKTEST_EXTSTAGE: where).
    if (const wchar_t* dp = _wgetenv(L"LOOKTEST_DUMPPAIR")) { SavePNG(std::wstring(dp) + L"/left.png", A, (UINT)ew, (UINT)eh); SavePNG(std::wstring(dp) + L"/right.png", B, (UINT)ew, (UINT)eh); }
    if (const wchar_t* ds = _wgetenv(L"LOOKTEST_DSP")) look.SetExternalDisparity(LoadDsp(dev, look, ds, ew, eh));
    if (const wchar_t* es = _wgetenv(L"LOOKTEST_EXTSTAGE")) look.SetExternalStage(_wtoi(es));
    if (const wchar_t* ff = _wgetenv(L"LOOKTEST_FARFILL")) look.SetFarFill(_wtoi(ff) != 0);
    if (const wchar_t* ge = _wgetenv(L"LOOKTEST_GEDGE")) look.SetGaussianEdge(_wtoi(ge) != 0);
    if (const wchar_t* gs = _wgetenv(L"LOOKTEST_GAUSS")) look.SetGaussian(_wtoi(gs), _wgetenv(L"LOOKTEST_GSIGMA") ? (float)_wtof(_wgetenv(L"LOOKTEST_GSIGMA")) : 0.5f, _wgetenv(L"LOOKTEST_GSEEN") ? (float)_wtof(_wgetenv(L"LOOKTEST_GSEEN")) : 0.25f);
    if (const wchar_t* fb = _wgetenv(L"LOOKTEST_FUSE")) look.SetFuseBias((float)_wtof(fb));
    if (const wchar_t* fe = _wgetenv(L"LOOKTEST_FUSEEDGE")) look.SetFuseEdge(_wtoi(fe) != 0);
    if (const wchar_t* fr = _wgetenv(L"LOOKTEST_FUSEREACH")) look.SetFuseReach((float)_wtof(fr));
    if (const wchar_t* xd = _wgetenv(L"LOOKTEST_DILATE")) look.SetExternalDilate(_wtoi(xd));
    if (const wchar_t* op = _wgetenv(L"LOOKTEST_OPT")) { float o4[4] = {}; swscanf_s(op, L"%f,%f,%f,%f", &o4[0], &o4[1], &o4[2], &o4[3]); for (int q = 0; q < 4; ++q) look.SetOpt(q, o4[q]); }

    // A moving picture, for the plate: LOOKTEST_SEQ=n,m gives n earlier pictures first, the camera m views
    // further left for each one back -- so what the views to the left of the eyes need was in sight
    // before. LOOKTEST_PLATE=0: the plate is kept but not filled from.
    if (const wchar_t* pt = _wgetenv(L"LOOKTEST_PLATE")) look.SetPlate(_wtoi(pt) != 0);
    if (const wchar_t* sq = _wgetenv(L"LOOKTEST_SEQ"))
    {
        int n = 0, m = 1; swscanf_s(sq, L"%d,%d", &n, &m);
        float sign = 1.0f; if (const wchar_t* sg = _wgetenv(L"LOOKTEST_SEQSIGN")) sign = (float)_wtof(sg);
        for (int k = n; k >= 0; --k)
        {
            const int va = ia - k * m, vb = ib - k * m;
            if (va < 0) continue;
            std::vector<uint8_t> a2, b2, s2((size_t)ew * 2 * eh * 4);
            view(va, a2); view(vb, b2);
            for (int y = 0; y < eh; ++y)
            {
                memcpy(&s2[(size_t)y * ew * 2 * 4], &a2[(size_t)y * ew * 4], (size_t)ew * 4);
                memcpy(&s2[((size_t)y * ew * 2 + ew) * 4], &b2[(size_t)y * ew * 4], (size_t)ew * 4);
            }
            D3D11_SUBRESOURCE_DATA sd2{ s2.data(), (UINT)(ew * 2 * 4), 0 };
            ID3D11Texture2D* t2 = nullptr; dev->CreateTexture2D(&td, &sd2, &t2);
            ID3D11ShaderResourceView* v2 = nullptr; dev->CreateShaderResourceView(t2, &vd, &v2);
            const bool known = _wgetenv(L"LOOKTEST_KNOWN") != nullptr;
            look.SetFindMotion(!known);
            if (k < n && known) look.SetMotion(0.0f, 0.0f, sign * (float)m / (float)step, 0.0f);
            look.Render(v2, ew, eh, 0.0f, 1.0f); ctx->Flush();
            { float mo[6]; if (look.ReadMotion(mo)) wprintf(L"motion found: shift %+.2f %+.2f px, per disparity %+.4f %+.4f (true %+.4f 0), misfit %.4f, counted %.2f\n", mo[0], mo[1], mo[2], mo[3], sign * (float)m / (float)step, mo[4], mo[5]); }
            v2->Release(); t2->Release();
        }
    }
    // The left half of the output read back (the left eye's new view).
    ID3D11Texture2D* staging = nullptr;
    auto readLeft = [&](std::vector<uint8_t>& out) {
        D3D11_TEXTURE2D_DESC od{}; look.OutputTexture()->GetDesc(&od);
        if (!staging) { od.Usage = D3D11_USAGE_STAGING; od.BindFlags = 0; od.CPUAccessFlags = D3D11_CPU_ACCESS_READ; od.MiscFlags = 0; od.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dev->CreateTexture2D(&od, nullptr, &staging); }
        ctx->CopyResource(staging, look.OutputTexture());
        D3D11_MAPPED_SUBRESOURCE m{}; ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
        out.resize((size_t)ew * eh * 4);
        for (int y = 0; y < eh; ++y) memcpy(&out[(size_t)y * ew * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)ew * 4);
        ctx->Unmap(staging, 0);
    };
    auto psnr = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        const int margin = ew / 16; double s = 0; size_t n = 0;
        for (int y = margin; y < eh - margin; ++y) for (int x = margin; x < ew - margin; ++x)
            for (int k = 0; k < 3; ++k) { const double d = (double)a[((size_t)y * ew + x) * 4 + k] - b[((size_t)y * ew + x) * 4 + k]; s += d * d; ++n; }
        return 10.0 * std::log10(255.0 * 255.0 / (std::max)(s / (std::max)(n, (size_t)1), 1e-9));
    };

    wprintf(L"quilt %dx%d views of %dx%d, enlarged x%d to %dx%d eyes; references %d and %d\n", C, R, vw, vh, scale, ew, eh, ia, ib);
    // LOOKTEST_DUMPDISP=file: the disparity in use (after everything), both eyes, as a .dsp at the map's
    // size and in the PICTURE's pixels -- to set beside another matcher's.
    if (const wchar_t* dd = _wgetenv(L"LOOKTEST_DUMPDISP"))
    {
        look.Render(srv, ew, eh, 0.3f, 1.3f);
        D3D11_TEXTURE2D_DESC d0{}; look.DisparityTexture()->GetDesc(&d0);
        D3D11_TEXTURE2D_DESC s0 = d0; s0.Usage = D3D11_USAGE_STAGING; s0.BindFlags = 0; s0.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* st0 = nullptr; dev->CreateTexture2D(&s0, nullptr, &st0);
        ctx->CopyResource(st0, look.DisparityTexture());
        D3D11_MAPPED_SUBRESOURCE m0{}; ctx->Map(st0, 0, D3D11_MAP_READ, 0, &m0);
        const int dw = (int)d0.Width / 2, dh = (int)d0.Height;
        auto hf = [](uint16_t hh) { const uint32_t s = (hh >> 15) & 1, e = (hh >> 10) & 31, mm = hh & 1023; float v = e == 0 ? std::ldexp((float)mm, -24) : std::ldexp((float)(mm | 1024), (int)e - 25); return s ? -v : v; };
        FILE* f = nullptr; _wfopen_s(&f, dd, L"wb");
        if (f)
        {
            fwrite("DSP1", 1, 4, f); fwrite(&dw, 4, 1, f); fwrite(&dh, 4, 1, f);
            for (int eye = 0; eye < 2; ++eye) for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x)
            {
                const uint16_t* p = (const uint16_t*)((const uint8_t*)m0.pData + (size_t)y * m0.RowPitch) + (size_t)(eye * dw + x) * 4;
                const float v = hf(p[0]) * (float)ew / (float)dw; fwrite(&v, 4, 1, f);
            }
            fclose(f);
        }
        ctx->Unmap(st0, 0); st0->Release();
    }
    // With no move, the picture must be the one that went in.
    { std::vector<uint8_t> o; look.Render(srv, ew, eh, 0.0f, 1.0f); readLeft(o); wprintf(L"no move: %.2f dB against the eye that went in (the higher the better; over 50 is the same picture)\n", psnr(o, A)); }

    double in = 0, edge = 0, far_ = 0; int nin = 0, nedge = 0, nfar = 0;
    std::vector<uint8_t> sheet; int rows = 0;
    wprintf(L"\n  view  position   PSNR dB\n");
    for (int v = (std::max)(0, ia - reach); v <= (std::min)(N - 1, ib + reach); ++v)
    {
        if (v == ia || v == ib) continue;
        const float t = (float)(v - ia) / (float)step;
        std::vector<uint8_t> truth, o; view(v, truth);
        look.Render(srv, ew, eh, t, 1.0f + t);
        readLeft(o);
        const double p = psnr(o, truth);
        if (_wgetenv(L"LOOKTEST_MARK"))
        {
            look.SetMarkFill(true); std::vector<uint8_t> mk; look.Render(srv, ew, eh, t, 1.0f + t); readLeft(mk); look.SetMarkFill(false);
            size_t np = 0, ng = 0; double se = 0, seAll = 0;
            for (size_t q = 0; q < (size_t)ew * eh; ++q)
            {
                const uint8_t* c = &mk[q * 4]; const bool pl = c[0] > 250 && c[1] < 5 && c[2] > 250, gr = c[0] < 5 && c[1] > 250 && c[2] < 5;
                double e = 0; for (int k = 0; k < 3; ++k) { const double d = (double)o[q * 4 + k] - truth[q * 4 + k]; e += d * d; }
                seAll += e; if (pl || gr) se += e; np += pl; ng += gr;
            }
            wprintf(L"        filled from the plate %.2f%%, guessed %.2f%%; those pixels carry %.0f%% of the error\n", 100.0 * np / ((double)ew * eh), 100.0 * ng / ((double)ew * eh), 100.0 * se / (std::max)(seAll, 1.0));
        }
        wprintf(L"  %4d  %+6.2f     %.2f\n", v, t, p);
        if (t > 0 && t < 1) { in += p; ++nin; } else if (t >= -0.5f && t <= 1.5f) { edge += p; ++nedge; } else { far_ += p; ++nfar; }
        if (!outdir.empty() && (v == (std::max)(0, ia - reach) || v == (ia + ib) / 2 || v == (std::min)(N - 1, ib + reach)))
        {
            // (Halved for looking at: the real view, then the one made.)
            const int sw = ew / 2, sh = eh / 2;
            sheet.resize((size_t)sw * 2 * sh * (rows + 1) * 4, 255);
            for (int y = 0; y < sh; ++y) for (int x = 0; x < sw; ++x)
            {
                memcpy(&sheet[((size_t)(rows * sh + y) * sw * 2 + x) * 4], &truth[((size_t)(y * 2) * ew + x * 2) * 4], 4);
                memcpy(&sheet[((size_t)(rows * sh + y) * sw * 2 + sw + x) * 4], &o[((size_t)(y * 2) * ew + x * 2) * 4], 4);
            }
            ++rows;
        }
    }
    wprintf(L"\nmean PSNR (dB): between %.2f | up to half outside %.2f | further out %.2f\n", nin ? in / nin : 0.0, nedge ? edge / nedge : 0.0, nfar ? far_ / nfar : 0.0);

    // GPU time: a run of frames, the stages as the class times them.
    {
        double sum[4] = {}; int n = 0;
        for (int f = 0; f < 200; ++f)
        {
            look.Render(srv, ew, eh, 0.3f, 1.3f);
            ctx->Flush();
            double ms[4]; if (look.TakeTimes(ms)) { for (int k = 0; k < 4; ++k) sum[k] += ms[k]; ++n; }
            if (f % 8 == 7) Sleep(5);
        }
        if (n) wprintf(L"GPU ms a frame (%dx%d eyes, mean of %d): levels %.2f, matching %.2f, geometry %.2f, new views %.2f | total %.2f\n",
                       ew, eh, n, sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, (sum[0] + sum[1] + sum[2] + sum[3]) / n);
    }
    if (!outdir.empty() && rows)
    {
        SavePNG(outdir + L"\\look_gpu.png", sheet, (UINT)(ew / 2 * 2), (UINT)(eh / 2 * rows));
        wprintf(L"written: look_gpu.png (left: the real view, right: the one made; rows: farthest left, middle, farthest right)\n");
    }
    look.Shutdown();
    return 0;
}
