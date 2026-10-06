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

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { fwprintf(stderr, L"usage: looktest <quilt_qsCxR.png> [refA refB] [reach] [scale] [outdir]\n"); return 1; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (argc >= 3 && !wcscmp(argv[1], L"sbs")) return RunSbs(argv[2], argc > 3 ? argv[3] : L"");
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
