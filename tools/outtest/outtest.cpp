// outtest: Stereo Loom's own display outputs (src/OutputEncoder), off the screen.
// A side-by-side picture -- left eye pure red-ish grey 200/50/50, right eye
// 50/50/200 -- drawn by each output into an 8 x 8 target, and the pixels that
// matter checked. Not part of the app:
//   cmake --build build/x64 --config Release --target outtest
#include "OutputEncoder.h"
#include <d3d11.h>
#include <cstdio>
#include <cstring>

using namespace srw;

static ID3D11Device* g_dev; static ID3D11DeviceContext* g_ctx;

static void Px(ID3D11Texture2D* rt, int x, int y, unsigned char out[4])
{
    D3D11_TEXTURE2D_DESC d{}; rt->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr; g_dev->CreateTexture2D(&d, nullptr, &st);
    g_ctx->CopyResource(st, rt);
    D3D11_MAPPED_SUBRESOURCE m{}; g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
    memcpy(out, (unsigned char*)m.pData + y * m.RowPitch + x * 4, 4);
    g_ctx->Unmap(st, 0); st->Release();
}

int main()
{
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    if (!g_dev) { printf("no device\n"); return 1; }
    // The input: 16 x 8, left half eye L, right half eye R; inside a 32 x 16 texture (larger than the picture).
    const int TW = 32, TH = 16;
    static unsigned int pix[TW * TH];
    for (int y = 0; y < TH; ++y) for (int x = 0; x < TW; ++x)
        pix[y * TW + x] = (x < 8 && y < 8) ? 0xFF3232C8u   // L: R=200 G=50 B=50 (ABGR)
                        : (x < 16 && y < 8) ? 0xFFC83232u  // R: R=50 G=50 B=200
                        : 0xFF00FF00u;                     // outside the picture: green (must never show)
    D3D11_TEXTURE2D_DESC td{}; td.Width = TW; td.Height = TH; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{ pix, TW * 4, 0 };
    ID3D11Texture2D* in = nullptr; g_dev->CreateTexture2D(&td, &init, &in);
    ID3D11ShaderResourceView* srv = nullptr; g_dev->CreateShaderResourceView(in, nullptr, &srv);
    td.Width = 8; td.Height = 8; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* rt = nullptr; g_dev->CreateTexture2D(&td, nullptr, &rt);
    ID3D11RenderTargetView* rtv = nullptr; g_dev->CreateRenderTargetView(rt, nullptr, &rtv);

    OutputEncoder enc;
    if (!enc.Create(g_ctx)) { printf("encoder not made\n"); return 1; }
    enc.SetInput(srv, 8, 8);
    int fails = 0;
    auto run = [&](DisplayOutput o, int col, int style, bool swap, int ox, int oy) {
        enc.SetOptions(o, col, style, swap);
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{ 0, 0, 8, 8, 0, 1 }; g_ctx->RSSetViewports(1, &vp);
        enc.Draw(ox, oy);
    };
    auto expect = [&](const char* what, int x, int y, int r, int g, int b) {
        unsigned char p[4]; Px(rt, x, y, p);
        const bool ok = abs(p[0] - r) <= 3 && abs(p[1] - g) <= 3 && abs(p[2] - b) <= 3;
        if (!ok) ++fails;
        printf("%-44s (%d,%d): %3d %3d %3d  want %3d %3d %3d  %s\n", what, x, y, p[0], p[1], p[2], r, g, b, ok ? "ok" : "FAIL");
    };
    run(DisplayOutput::SideBySide, 0, 0, false, 0, 0);
    expect("Side-by-Side: left half is L", 1, 4, 200, 50, 50);
    expect("Side-by-Side: right half is R", 6, 4, 50, 50, 200);
    run(DisplayOutput::SideBySide, 0, 0, true, 0, 0);
    expect("Side-by-Side swapped: left half is R", 1, 4, 50, 50, 200);
    run(DisplayOutput::TopAndBottom, 0, 0, false, 0, 0);
    expect("Top-and-Bottom: top is L", 4, 1, 200, 50, 50);
    expect("Top-and-Bottom: bottom is R", 4, 6, 50, 50, 200);
    run(DisplayOutput::RowInterleaved, 0, 0, false, 0, 0);
    expect("Rows: row 0 is L", 3, 0, 200, 50, 50);
    expect("Rows: row 1 is R", 3, 1, 50, 50, 200);
    run(DisplayOutput::RowInterleaved, 0, 0, false, 0, 1);
    expect("Rows, window one row down: row 0 is R", 3, 0, 50, 50, 200);
    run(DisplayOutput::ColumnInterleaved, 0, 0, false, 0, 0);
    expect("Columns: column 0 is L", 0, 3, 200, 50, 50);
    expect("Columns: column 1 is R", 1, 3, 50, 50, 200);
    run(DisplayOutput::Checkerboard, 0, 0, false, 0, 0);
    expect("Checkerboard: (0,0) is L", 0, 0, 200, 50, 50);
    expect("Checkerboard: (1,0) is R", 1, 0, 50, 50, 200);
    expect("Checkerboard: (1,1) is L", 1, 1, 200, 50, 50);
    run(DisplayOutput::Anaglyph, 0, 1, false, 0, 0);
    expect("Anaglyph red/cyan Colour", 4, 4, 200, 50, 200);
    run(DisplayOutput::Anaglyph, 0, 3, false, 0, 0);
    // (Grey: red = L's brightness 0.299*200+0.587*50+0.114*50 = 94.9; green, blue = R's 0.299*50+0.587*50+0.114*200 = 67.1.)
    expect("Anaglyph red/cyan Grey", 4, 4, 95, 67, 67);
    run(DisplayOutput::Anaglyph, 2, 1, false, 0, 0);
    expect("Anaglyph amber/blue Colour", 4, 4, 200, 50, 200);
    run(DisplayOutput::Anaglyph, 1, 1, true, 0, 0);
    expect("Anaglyph green/magenta Colour, swapped", 4, 4, 200, 50, 50);
    run(DisplayOutput::Anaglyph, 0, 0, false, 0, 0);
    // (Dubois: red .456*200+.5*50+.176*50 - (.043*50+.088*50+.002*200) = 91.2+25+8.8-6.95-.4 = 117.7;
    //  green -.04*200-.038*50-.016*50 + .378*50+.734*50-.018*200 = -10.7+55.6-3.6 = 41.3;
    //  blue -.015*200-.021*50-.005*50 + (-.072*50-.113*50+1.226*200) = -4.3+(-9.25+245.2) = 231.65)
    expect("Anaglyph red/cyan Optimised (Dubois)", 4, 4, 118, 41, 232);
    printf("%d failed\n", fails);
    return fails ? 1 : 0;
}
