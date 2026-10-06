// splatlab: the look-around experiment, offline (see lib\"Real-time Gaussian
// splats from stereo for SR Loom.md", Phase 0). Two views of a scene go in;
// views from other places come out, by three methods, and each is scored
// against the real picture from that place.
//
// The test material is Looking Glass quilts: dozens of real views of one scene,
// a fixed step apart along a line. Two of them are taken as the "stereo pair"
// (A at position 0, B at position 1); every view between them and to either
// side is then synthesised and compared with the quilt's own.
//
//   splatlab <quilt_qsCxR.png> [refA refB] [reach] [outdir]
//
// refA / refB: the two view numbers (default: 4 either side of the middle).
// reach: how many views past each reference to try (default: the references'
// spacing -- positions -1 .. 2). outdir: pictures to look at are written there.
//
// Everything here is plain CPU code, for finding out what works. What works
// moves to shaders.
//
// Conventions. A point seen at xA in view A is seen at xB = xA - d in view B:
// d is its disparity, in pixels, positive or negative (the views converge on a
// plane; what is behind it has the other sign). From position t the point is
// at xA - t d. Larger d = nearer.
#include <windows.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

struct Img { int w = 0, h = 0; std::vector<float> p; float* at(int x, int y) { return &p[((size_t)y * w + x) * 3]; } const float* at(int x, int y) const { return &p[((size_t)y * w + x) * 3]; } };
struct Map { int w = 0, h = 0; std::vector<float> v; float& at(int x, int y) { return v[(size_t)y * w + x]; } float at(int x, int y) const { return v[(size_t)y * w + x]; } };

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

// Rows y0..y1 handed out to every core.
template <typename F> static void Rows(int h, F&& f)
{
    const int n = (std::max)(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> th;
    for (int k = 0; k < n; ++k) th.emplace_back([&, k] { for (int y = k; y < h; y += n) f(y); });
    for (auto& t : th) t.join();
}

static inline float Luma(const float* c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; }
static inline void Bilinear(const Img& s, float x, float y, float* o)
{
    x = (std::max)(0.0f, (std::min)((float)s.w - 1.001f, x)); y = (std::max)(0.0f, (std::min)((float)s.h - 1.001f, y));
    const int x0 = (int)x, y0 = (int)y; const float fx = x - x0, fy = y - y0;
    const float* a = s.at(x0, y0); const float* b = s.at(x0 + 1, y0); const float* c = s.at(x0, y0 + 1); const float* d = s.at(x0 + 1, y0 + 1);
    for (int k = 0; k < 3; ++k) o[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
}
static inline float MapAt(const Map& m, float x, float y)
{
    x = (std::max)(0.0f, (std::min)((float)m.w - 1.001f, x)); y = (std::max)(0.0f, (std::min)((float)m.h - 1.001f, y));
    const int x0 = (int)x, y0 = (int)y; const float fx = x - x0, fy = y - y0;
    return (m.at(x0, y0) * (1 - fx) + m.at(x0 + 1, y0) * fx) * (1 - fy) + (m.at(x0, y0 + 1) * (1 - fx) + m.at(x0 + 1, y0 + 1) * fx) * fy;
}

// ---- Disparity between two full-colour views --------------------------------
// Coarse to fine, as SR Loom's own anaglyph matcher goes: the pictures halved
// three times; at the smallest every disparity in range is tried, and each
// larger size only looks a little either side of what the size below found --
// and of what its neighbours found, so a thin thing lost below can be picked up
// again. A plain or blurred area, where nothing can be matched at full size,
// takes its answer from the sizes where its surroundings are in the window.
// After each size the two maps are checked against each other and cleaned.
//
// Cost of a candidate: colour difference and horizontal-gradient difference,
// each capped (one bad pixel cannot decide it), summed over a 7x7 window.
struct Level { Img img; Map luma, grad; };
static Level MakeLevel(const Img& s)
{
    Level l; l.img = s;
    const int W = s.w, H = s.h;
    l.luma.w = l.grad.w = W; l.luma.h = l.grad.h = H; l.luma.v.resize((size_t)W * H); l.grad.v.resize((size_t)W * H);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) l.luma.at(x, y) = Luma(s.at(x, y));
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) l.grad.at(x, y) = l.luma.at((std::min)(x + 1, W - 1), y) - l.luma.at((std::max)(x - 1, 0), y);
    return l;
}
static Img Halve(const Img& s)
{
    Img o; o.w = (std::max)(1, s.w / 2); o.h = (std::max)(1, s.h / 2); o.p.resize((size_t)o.w * o.h * 3);
    for (int y = 0; y < o.h; ++y) for (int x = 0; x < o.w; ++x)
        for (int k = 0; k < 3; ++k)
            o.at(x, y)[k] = 0.25f * (s.at(2 * x, 2 * y)[k] + s.at((std::min)(2 * x + 1, s.w - 1), 2 * y)[k] +
                                     s.at(2 * x, (std::min)(2 * y + 1, s.h - 1))[k] + s.at((std::min)(2 * x + 1, s.w - 1), (std::min)(2 * y + 1, s.h - 1))[k]);
    return o;
}
static inline float Cost(const Level& a, const Level& b, int x, int y, int d, float sign, int R)
{
    const int W = a.img.w, H = a.img.h; float s = 0;
    for (int j = -R; j <= R; ++j)
    {
        const int yy = (std::max)(0, (std::min)(H - 1, y + j));
        for (int i = -R; i <= R; ++i)
        {
            const int xa = (std::max)(0, (std::min)(W - 1, x + i)), xb = xa - (int)(sign * d);
            if (xb < 0 || xb >= W) { s += 14.0f; continue; }
            const float* ca = a.img.at(xa, yy); const float* cb = b.img.at(xb, yy);
            const float ad = (std::fabs(ca[0] - cb[0]) + std::fabs(ca[1] - cb[1]) + std::fabs(ca[2] - cb[2])) / 3.0f;
            const float gd = std::fabs(a.grad.at(xa, yy) - b.grad.at(xb, yy));
            s += 0.35f * (std::min)(ad, 30.0f) + 0.65f * (std::min)(gd, 20.0f);
        }
    }
    return s;
}
// One size: for each pixel of `a`, the best of its candidates. start: the size
// below's answer, doubled (empty: every disparity dmin..dmax is tried).
static void MatchLevel(const Level& a, const Level& b, float sign, const Map* start, int dmin, int dmax, Map& disp, Map& conf)
{
    const int W = a.img.w, H = a.img.h, R = 3;
    disp.w = conf.w = W; disp.h = conf.h = H; disp.v.assign((size_t)W * H, 0.0f); conf.v.assign((size_t)W * H, 0.0f);
    Rows(H, [&](int y) {
        std::vector<int> cand;
        for (int x = 0; x < W; ++x)
        {
            cand.clear();
            if (!start) { for (int d = dmin; d <= dmax; ++d) cand.push_back(d); }
            else
            {
                // (Its own start, two either side; and its neighbours' starts.)
                static const int nx[5] = { 0, -3, 3, 0, 0 }, ny[5] = { 0, 0, 0, -3, 3 };
                for (int n = 0; n < 5; ++n)
                {
                    const int c = (int)std::lround(start->at((std::max)(0, (std::min)(W - 1, x + nx[n])), (std::max)(0, (std::min)(H - 1, y + ny[n]))));
                    const int reach = n == 0 ? 2 : 0;
                    for (int k = -reach; k <= reach; ++k)
                    {
                        const int d = (std::max)(dmin, (std::min)(dmax, c + k));
                        if (std::find(cand.begin(), cand.end(), d) == cand.end()) cand.push_back(d);
                    }
                }
            }
            float best = 1e30f, second = 1e30f; int bd = 0;
            for (int d : cand)
            {
                const float c = Cost(a, b, x, y, d, sign, R);
                if (c < best) { if (std::abs(bd - d) > 1) second = best; best = c; bd = d; }
                else if (c < second && std::abs(bd - d) > 1) second = c;
            }
            // (Between candidates: a parabola through the best and its two neighbours.)
            const float cm = Cost(a, b, x, y, bd - 1, sign, R), cp = Cost(a, b, x, y, bd + 1, sign, R);
            const float den = cm - 2.0f * best + cp;
            const float sub = den > 1e-4f ? (std::max)(-0.5f, (std::min)(0.5f, 0.5f * (cm - cp) / den)) : 0.0f;
            disp.at(x, y) = (float)bd + sub;
            // (How much there was to match on here: the cost's rise either side of the best.)
            conf.at(x, y) = (std::max)(0.0f, (std::min)(1.0f, den / (49.0f * 2.0f)));
        }
    });
}

// The two maps checked against each other: a pixel of A at d must land on a
// pixel of B that points back (within a pixel). What fails is hidden in the
// other view, or wrong: its confidence goes to 0, and its disparity is taken
// from the row's nearest pixels that passed -- the FARTHER of the two sides,
// since what is hidden from the other eye is background.
static void CrossCheck(Map& dA, Map& cA, const Map& dB, float signA)
{
    const int W = dA.w, H = dA.h;
    std::vector<uint8_t> ok((size_t)W * H);
    Rows(H, [&](int y) {
        for (int x = 0; x < W; ++x)
        {
            const float d = dA.at(x, y); const float xb = x - signA * d;
            bool good = xb >= 0 && xb <= W - 1;
            if (good) good = std::fabs(MapAt(dB, xb, (float)y) - d) < 1.0f;
            ok[(size_t)y * W + x] = good;
        }
    });
    Rows(H, [&](int y) {
        for (int x = 0; x < W; ++x)
        {
            if (ok[(size_t)y * W + x]) continue;
            int l = x - 1; while (l >= 0 && !ok[(size_t)y * W + l]) --l;
            int r = x + 1; while (r < W && !ok[(size_t)y * W + r]) ++r;
            float v = 0; bool have = false;
            if (l >= 0) { v = dA.at(l, y); have = true; }
            if (r < W) { v = have ? (std::min)(v, dA.at(r, y)) : dA.at(r, y); have = true; }
            if (have) dA.at(x, y) = v;
            cA.at(x, y) = 0.0f;
        }
    });
}

// A 5x5 median: single wrong pixels out, edges kept.
static void Median(Map& m)
{
    Map o = m;
    Rows(m.h, [&](int y) {
        float v[25];
        for (int x = 0; x < m.w; ++x)
        {
            int n = 0;
            for (int j = -2; j <= 2; ++j) for (int i = -2; i <= 2; ++i)
                v[n++] = m.at((std::max)(0, (std::min)(m.w - 1, x + i)), (std::max)(0, (std::min)(m.h - 1, y + j)));
            std::nth_element(v, v + 12, v + 25);
            o.at(x, y) = v[12];
        }
    });
    m = o;
}
static Map Double(const Map& s, int w, int h)
{
    Map o; o.w = w; o.h = h; o.v.resize((size_t)w * h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) o.at(x, y) = 2.0f * s.at((std::min)(x / 2, s.w - 1), (std::min)(y / 2, s.h - 1));
    return o;
}
// Both ways, all sizes. range: the largest disparity either way, in pixels of the full size.
static void MatchPair(const Img& A, const Img& B, int range, Map& dA, Map& cA, Map& dB, Map& cB)
{
    const int kLevels = 4;
    std::vector<Level> la, lb;
    { Img a = A, b = B; for (int l = 0; l < kLevels; ++l) { la.push_back(MakeLevel(a)); lb.push_back(MakeLevel(b)); a = Halve(a); b = Halve(b); } }
    Map sa, sb;
    for (int l = kLevels - 1; l >= 0; --l)
    {
        const int r = range >> l;
        const bool first = l == kLevels - 1;
        Map ia, ib;
        if (!first) { ia = Double(sa, la[l].img.w, la[l].img.h); ib = Double(sb, lb[l].img.w, lb[l].img.h); }
        MatchLevel(la[l], lb[l], +1.0f, first ? nullptr : &ia, -r - 1, r + 1, dA, cA);   // xB = xA - d
        MatchLevel(lb[l], la[l], -1.0f, first ? nullptr : &ib, -r - 1, r + 1, dB, cB);   // xA = xB + d
        { Map a0 = dA; CrossCheck(dA, cA, dB, +1.0f); CrossCheck(dB, cB, a0, -1.0f); }
        Median(dA); Median(dB);
        sa = dA; sb = dB;
    }
}

// ---- The three ways of making a view from position t ------------------------
struct Ref { const Img* img; const Map* disp; const Map* conf; float pos; };   // (pos: 0 for A, 1 for B)

// The shift of a reference's pixel as seen from t: A's go by -(t) d, B's by +(1 - t) d.
static inline float Shift(const Ref& r, float t) { return r.pos - t; }

// 1. Backward warp from ONE view (what RGB + Depth does today): for each pixel
// of the new view, where in the reference it comes from is found by going
// round a few times -- x = x' - shift * d(x). No test for what is hidden:
// at a depth edge it stretches the picture.
static void WarpOne(const Ref& r, float t, Img& out, Map* depthOut = nullptr, Map* okOut = nullptr)
{
    const int W = r.img->w, H = r.img->h; const float s = Shift(r, t);
    out.w = W; out.h = H; out.p.assign((size_t)W * H * 3, 0.0f);
    if (depthOut) { depthOut->w = W; depthOut->h = H; depthOut->v.assign((size_t)W * H, 0.0f); }
    if (okOut) { okOut->w = W; okOut->h = H; okOut->v.assign((size_t)W * H, 0.0f); }
    Rows(H, [&](int y) {
        for (int x = 0; x < W; ++x)
        {
            float xs = (float)x;
            for (int it = 0; it < 6; ++it) xs = (float)x - s * MapAt(*r.disp, xs, (float)y);
            const float d = MapAt(*r.disp, xs, (float)y);
            Bilinear(*r.img, xs, (float)y, out.at(x, y));
            if (depthOut) depthOut->at(x, y) = d;
            // (Settled, and inside the picture: else this pixel is a guess.)
            if (okOut) okOut->at(x, y) = (std::fabs(xs + s * d - (float)x) < 0.75f && xs >= 0 && xs <= W - 1) ? 1.0f : 0.0f;
        }
    });
}

// 2. Backward warp from BOTH views, with a test: each view's answer is kept
// only where it settled; where both did and they agree on the depth they are
// mixed by nearness to t; where they disagree the NEARER surface wins (it
// hides the other); where neither did, the farther of the two guesses.
static void WarpTwo(const Ref& a, const Ref& b, float t, Img& out, Map& holes)
{
    Img ia, ib; Map da, db, oa, ob;
    WarpOne(a, t, ia, &da, &oa); WarpOne(b, t, ib, &db, &ob);
    const int W = ia.w, H = ia.h;
    out.w = W; out.h = H; out.p.assign((size_t)W * H * 3, 0.0f);
    holes.w = W; holes.h = H; holes.v.assign((size_t)W * H, 0.0f);
    const float wa = (std::max)(0.02f, (std::min)(1.0f, 1.0f - std::fabs(t - a.pos))), wb = (std::max)(0.02f, (std::min)(1.0f, 1.0f - std::fabs(t - b.pos)));
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
    {
        const bool va = oa.at(x, y) > 0.5f, vb = ob.at(x, y) > 0.5f;
        const float* ca = ia.at(x, y); const float* cb = ib.at(x, y); float* o = out.at(x, y);
        float fa = 0, fb = 0;
        if (va && vb)
        {
            if (std::fabs(da.at(x, y) - db.at(x, y)) < 1.5f) { fa = wa; fb = wb; }
            else if (da.at(x, y) > db.at(x, y)) fa = 1; else fb = 1;
        }
        else if (va) fa = 1; else if (vb) fb = 1;
        else { holes.at(x, y) = 1.0f; if (da.at(x, y) < db.at(x, y)) fa = 1; else fb = 1; }
        for (int k = 0; k < 3; ++k) o[k] = (ca[k] * fa + cb[k] * fb) / (fa + fb);
    }
}

// 3. Forward splats: every pixel of both views is a small Gaussian, placed
// where it is seen from t, stretched along its surface's slope (so a slanted
// wall stays closed) but never across a depth edge (so a gap opens there
// instead of a smear), its weight its match confidence. Visibility by a soft
// depth buffer, no sorting: first the nearest depth at each pixel, then the sum
// of everything within a little of it. What stays empty is filled from the
// FARTHER side along the row.
static void Splat(const std::vector<Ref>& refs, float t, Img& out, Map& holes, float sigma = 0.6f)
{
    const int W = refs[0].img->w, H = refs[0].img->h;
    out.w = W; out.h = H; out.p.assign((size_t)W * H * 3, 0.0f);
    holes.w = W; holes.h = H; holes.v.assign((size_t)W * H, 0.0f);
    std::vector<float> zNear((size_t)W * H, -1e30f), wsum((size_t)W * H, 0.0f), dsum((size_t)W * H, 0.0f);
    const float eps = 1.0f;   // (px of disparity: what counts as the same surface)
    struct Frag { float x, sx, d, w; const float* c; int y; };
    for (int pass = 0; pass < 2; ++pass)
        for (const Ref& r : refs)
        {
            const float s = Shift(r, t);
            const float wr = (std::max)(0.05f, (std::min)(1.0f, 1.0f - std::fabs(t - r.pos)));
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
            {
                const float d = r.disp->at(x, y);
                const float cf = r.conf ? r.conf->at(x, y) : 1.0f;
                // (The slope along the row, from the neighbours on the same surface.)
                const float dl = r.disp->at((std::max)(x - 1, 0), y), dr = r.disp->at((std::min)(x + 1, W - 1), y);
                float g = 0.5f * (dr - dl);
                if (std::fabs(dr - d) > 1.0f || std::fabs(d - dl) > 1.0f) g = 0.0f;   // (a depth edge: no bridging)
                const float xt = (float)x + s * d;
                const float stretch = (std::max)(0.5f, (std::min)(3.0f, std::fabs(1.0f + s * g)));
                const float sx = sigma * stretch, sy = sigma;
                const float op = (0.15f + 0.85f * cf) * wr;
                const int x0 = (int)std::floor(xt - 3 * sx), x1 = (int)std::ceil(xt + 3 * sx);
                for (int yy = (std::max)(0, y - 1); yy <= (std::min)(H - 1, y + 1); ++yy)
                    for (int xx = (std::max)(0, x0); xx <= (std::min)(W - 1, x1); ++xx)
                    {
                        const float ex = (xx - xt) / sx, ey = (float)(yy - y) / sy;
                        const float g2 = std::exp(-0.5f * (ex * ex + ey * ey));
                        if (g2 < 0.02f) continue;
                        const size_t i = (size_t)yy * W + xx;
                        if (pass == 0) { if (d > zNear[i]) zNear[i] = d; }
                        else if (d >= zNear[i] - eps)
                        {
                            const float w = g2 * op;
                            const float* c = r.img->at(x, y); float* o = out.at(xx, yy);
                            o[0] += c[0] * w; o[1] += c[1] * w; o[2] += c[2] * w; wsum[i] += w; dsum[i] += d * w;
                        }
                    }
            }
        }
    for (size_t i = 0; i < wsum.size(); ++i)
        if (wsum[i] > 0.02f) { out.p[i * 3] /= wsum[i]; out.p[i * 3 + 1] /= wsum[i]; out.p[i * 3 + 2] /= wsum[i]; dsum[i] /= wsum[i]; }
        else { holes.v[i] = 1.0f; }
    // (Holes: from the farther side along the row.)
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
    {
        if (holes.at(x, y) < 0.5f) continue;
        int l = x - 1; while (l >= 0 && holes.at(l, y) > 0.5f) --l;
        int r = x + 1; while (r < W && holes.at(r, y) > 0.5f) ++r;
        int from = -1;
        if (l >= 0 && r < W) from = dsum[(size_t)y * W + l] < dsum[(size_t)y * W + r] ? l : r;
        else if (l >= 0) from = l; else if (r < W) from = r;
        if (from >= 0) for (int k = 0; k < 3; ++k) out.at(x, y)[k] = out.at(from, y)[k];
    }
}

// ---- Scoring ----------------------------------------------------------------
static double Psnr(const Img& a, const Img& b, int margin)
{
    double s = 0; size_t n = 0;
    for (int y = margin; y < a.h - margin; ++y) for (int x = margin; x < a.w - margin; ++x)
        for (int k = 0; k < 3; ++k) { const double d = a.at(x, y)[k] - b.at(x, y)[k]; s += d * d; ++n; }
    return 10.0 * std::log10(255.0 * 255.0 / (std::max)(s / (std::max)(n, (size_t)1), 1e-9));
}
static double Share(const Map& m) { double s = 0; for (float v : m.v) s += v; return 100.0 * s / (std::max)(m.v.size(), (size_t)1); }
static void Put(std::vector<uint8_t>& sheet, int sw, const Img& im, int ox, int oy)
{
    for (int y = 0; y < im.h; ++y) for (int x = 0; x < im.w; ++x)
    {
        uint8_t* o = &sheet[((size_t)(oy + y) * sw + ox + x) * 4];
        for (int k = 0; k < 3; ++k) o[k] = (uint8_t)(std::max)(0.0f, (std::min)(255.0f, im.at(x, y)[k] + 0.5f));
        o[3] = 255;
    }
}

// ---- An ordinary stereo pair ------------------------------------------------
// Two real views and nothing else (a game's side-by-side frame, a photo pair):
// what SR Loom's converter hands on for every two-view format. There is no
// third view to score against, so each eye is predicted from the OTHER eye
// alone and compared with the real one -- a whole eye spacing's move, from one
// view: harder than anything look-around asks. Then views from beside and
// between the pair are written out to look at.
static Img FromRGBA(const std::vector<uint8_t>& px, int W, int x0, int y0, int w, int h)
{
    Img im; im.w = w; im.h = h; im.p.resize((size_t)w * h * 3);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
    {
        const uint8_t* s = &px[((size_t)(y0 + y) * W + x0 + x) * 4];
        float* d = im.at(x, y); d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
    }
    return im;
}
static double BadShare(const Img& a, const Img& b, int margin)
{
    size_t bad = 0, n = 0;
    for (int y = margin; y < a.h - margin; ++y) for (int x = margin; x < a.w - margin; ++x, ++n)
    {
        float m = 0; for (int k = 0; k < 3; ++k) m = (std::max)(m, std::fabs(a.at(x, y)[k] - b.at(x, y)[k]));
        bad += m > 40.0f;
    }
    return 100.0 * bad / (std::max)(n, (size_t)1);
}
static int PairMode(Img A, Img B, const std::wstring& outdir)
{
    // (Worked on at 720 px across at most: this is slow CPU code.)
    while (A.w > 720) { A = Halve(A); B = Halve(B); }
    const int W = A.w, H = A.h;
    const int range = _wgetenv(L"SPLATLAB_RANGE") ? _wtoi(_wgetenv(L"SPLATLAB_RANGE")) : (std::max)(8, W / 8);
    Map dA, cA, dB, cB;
    MatchPair(A, B, range, dA, cA, dB, cB);
    double lo = 1e9, hi = -1e9, okc = 0;
    for (size_t i = 0; i < dA.v.size(); ++i) { lo = (std::min)(lo, (double)dA.v[i]); hi = (std::max)(hi, (double)dA.v[i]); okc += cA.v[i] > 0.0f; }
    wprintf(L"pair %dx%d; disparity %.1f .. %.1f px, %.1f%% of pixels matched both ways\n", W, H, lo, hi, 100.0 * okc / dA.v.size());
    const Ref ra{ &A, &dA, &cA, 0.0f }, rb{ &B, &dB, &cB, 1.0f };
    const int margin = W / 12;
    // Each eye from the other alone.
    Img w1, w2, s1, s2; Map h1, h2;
    WarpOne(ra, 1.0f, w1); WarpOne(rb, 0.0f, w2);
    Splat({ ra }, 1.0f, s1, h1); Splat({ rb }, 0.0f, s2, h2);
    wprintf(L"\neach eye made from the other eye alone       PSNR dB   pixels off by > 40\n");
    wprintf(L"  warp:   right from left / left from right   %.2f / %.2f   %.2f%% / %.2f%%\n", Psnr(w1, B, margin), Psnr(w2, A, margin), BadShare(w1, B, margin), BadShare(w2, A, margin));
    wprintf(L"  splats: right from left / left from right   %.2f / %.2f   %.2f%% / %.2f%%   (holes before filling %.1f%% / %.1f%%)\n",
            Psnr(s1, B, margin), Psnr(s2, A, margin), BadShare(s1, B, margin), BadShare(s2, A, margin), Share(h1), Share(h2));
    if (!outdir.empty())
    {
        // Rows: from half a spacing to the left of the pair, between them, half a
        // spacing to the right. Columns: warp from the nearer view, two-view warp, splats.
        const float ts[3] = { -0.5f, 0.5f, 1.5f };
        std::vector<uint8_t> sheet((size_t)W * 3 * H * 3 * 4, 0);
        for (int r = 0; r < 3; ++r)
        {
            Img o1, o2, o3; Map hh2, hh3;
            WarpOne(ts[r] < 0.5f ? ra : rb, ts[r], o1); WarpTwo(ra, rb, ts[r], o2, hh2); Splat({ ra, rb }, ts[r], o3, hh3);
            Put(sheet, W * 3, o1, 0, H * r); Put(sheet, W * 3, o2, W, H * r); Put(sheet, W * 3, o3, W * 2, H * r);
        }
        SavePNG(outdir + L"\\pair_views.png", sheet, (UINT)(W * 3), (UINT)(H * 3));
        // (The disparity found for the left view, as a grey picture: near is light.)
        std::vector<uint8_t> dm((size_t)W * H * 4);
        for (int i = 0; i < W * H; ++i) { const uint8_t g = (uint8_t)(std::max)(0.0, (std::min)(255.0, 255.0 * (dA.v[i] - lo) / (std::max)(hi - lo, 1e-3))); dm[i * 4] = dm[i * 4 + 1] = dm[i * 4 + 2] = g; dm[i * 4 + 3] = 255; }
        SavePNG(outdir + L"\\pair_disparity.png", dm, (UINT)W, (UINT)H);
        wprintf(L"written: pair_views.png (rows: half a spacing left, between, half a spacing right; columns: warp, two-view warp, splats) and pair_disparity.png\n");
    }
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        fwprintf(stderr, L"usage: splatlab <quilt_qsCxR.png> [refA refB] [reach] [outdir]\n"
                         L"       splatlab pair <left.png> <right.png> [outdir]\n"
                         L"       splatlab sbs <side-by-side.png> [outdir]      (left eye in the left half)\n");
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (argc >= 4 && !wcscmp(argv[1], L"pair"))
    {
        std::vector<uint8_t> l, r; UINT lw = 0, lh = 0, rw = 0, rh = 0;
        if (!LoadRGBA(argv[2], l, lw, lh) || !LoadRGBA(argv[3], r, rw, rh) || lw != rw || lh != rh) { fwprintf(stderr, L"the two pictures must load and match in size\n"); return 1; }
        return PairMode(FromRGBA(l, (int)lw, 0, 0, (int)lw, (int)lh), FromRGBA(r, (int)rw, 0, 0, (int)rw, (int)rh), argc > 4 ? argv[4] : L"");
    }
    if (argc >= 3 && !wcscmp(argv[1], L"sbs"))
    {
        std::vector<uint8_t> s; UINT sw = 0, sh = 0;
        if (!LoadRGBA(argv[2], s, sw, sh)) { fwprintf(stderr, L"can't load the picture\n"); return 1; }
        return PairMode(FromRGBA(s, (int)sw, 0, 0, (int)sw / 2, (int)sh), FromRGBA(s, (int)sw, (int)sw / 2, 0, (int)sw / 2, (int)sh), argc > 3 ? argv[3] : L"");
    }
    std::wstring path = argv[1];
    int C = 8, R = 6;
    { const size_t q = path.rfind(L"qs"); if (q != std::wstring::npos) swscanf_s(path.c_str() + q, L"qs%dx%d", &C, &R); }
    std::vector<uint8_t> px; UINT qw = 0, qh = 0;
    if (!LoadRGBA(path.c_str(), px, qw, qh)) { fwprintf(stderr, L"can't load %s\n", path.c_str()); return 1; }
    const int N = C * R, vw = (int)qw / C, vh = (int)qh / R;
    int ia = argc > 3 ? _wtoi(argv[2]) : N / 2 - 4, ib = argc > 3 ? _wtoi(argv[3]) : N / 2 + 4;
    const int step = ib - ia;
    const int reach = argc > 4 ? _wtoi(argv[4]) : step;
    const std::wstring outdir = argc > 5 ? argv[5] : L"";
    if (ia < 0 || ib >= N || step <= 0) { fwprintf(stderr, L"bad reference views\n"); return 1; }
    // (View 0 is the bottom-left tile; they run left to right, then up.)
    auto view = [&](int v) {
        Img im; im.w = vw; im.h = vh; im.p.resize((size_t)vw * vh * 3);
        const int col = v % C, row = R - 1 - v / C;
        for (int y = 0; y < vh; ++y) for (int x = 0; x < vw; ++x)
        {
            const uint8_t* s = &px[((size_t)(row * vh + y) * qw + col * vw + x) * 4];
            float* d = im.at(x, y); d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        }
        return im;
    };
    const Img A = view(ia), B = view(ib);
    wprintf(L"quilt %dx%d views of %dx%d; references %d and %d (%d apart), targets %d .. %d\n", C, R, vw, vh, ia, ib, step, (std::max)(0, ia - reach), (std::min)(N - 1, ib + reach));

    // Disparity both ways, checked against each other, cleaned.
    // (How far apart a point can be in the two views: a fifth of the width,
    // or SPLATLAB_RANGE px.)
    const int range = _wgetenv(L"SPLATLAB_RANGE") ? _wtoi(_wgetenv(L"SPLATLAB_RANGE")) : (std::max)(8, vw / 5);
    Map dA, cA, dB, cB;
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    MatchPair(A, B, range, dA, cA, dB, cB);
    QueryPerformanceCounter(&t1);
    { double lo = 1e9, hi = -1e9, okc = 0; for (size_t i = 0; i < dA.v.size(); ++i) { lo = (std::min)(lo, (double)dA.v[i]); hi = (std::max)(hi, (double)dA.v[i]); okc += cA.v[i] > 0.0f; }
      wprintf(L"disparity: %.1f .. %.1f px, %.1f%% of pixels matched both ways (CPU %.0f ms -- not the point here)\n", lo, hi, 100.0 * okc / dA.v.size(), (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart); }

    const Ref ra{ &A, &dA, &cA, 0.0f }, rb{ &B, &dB, &cB, 1.0f };
    const std::vector<Ref> both{ ra, rb };
    struct Acc { double mid = 0, edge = 0, out = 0; int nin = 0, nnear = 0, nfar = 0; double holes = 0; int nh = 0; } acc[3];
    const wchar_t* names[3] = { L"1 warp from the nearer view (as RGB + Depth today)", L"2 warp from both views, hidden parts tested", L"3 splats from both views, soft depth buffer" };
    wprintf(L"\n  view  position   %-10s %-10s %-10s   (PSNR dB against the quilt's own view; holes %% for 2 and 3)\n", L"1 warp", L"2 two-view", L"3 splats");
    std::vector<uint8_t> sheet; int sheetRows = 0; const int sheetW = vw * 4;
    for (int v = (std::max)(0, ia - reach); v <= (std::min)(N - 1, ib + reach); ++v)
    {
        if (v == ia || v == ib) continue;
        const float t = (float)(v - ia) / (float)step;
        const Img truth = view(v);
        Img o1, o2, o3; Map h2, h3;
        WarpOne(t < 0.5f ? ra : rb, t, o1);
        WarpTwo(ra, rb, t, o2, h2);
        Splat(both, t, o3, h3);
        const int margin = vw / 16;   // (the picture's edges have nothing to come from)
        const double p[3] = { Psnr(o1, truth, margin), Psnr(o2, truth, margin), Psnr(o3, truth, margin) };
        wprintf(L"  %4d  %+6.2f     %-10.2f %-10.2f %-10.2f   %.1f%% / %.1f%%\n", v, t, p[0], p[1], p[2], Share(h2), Share(h3));
        const bool inside = t > 0 && t < 1, nearOut = !inside && (t >= -0.5f && t <= 1.5f);
        for (int k = 0; k < 3; ++k)
        {
            if (inside) { acc[k].mid += p[k]; ++acc[k].nin; } else if (nearOut) { acc[k].edge += p[k]; ++acc[k].nnear; } else { acc[k].out += p[k]; ++acc[k].nfar; }
        }
        acc[1].holes += Share(h2); ++acc[1].nh; acc[2].holes += Share(h3); ++acc[2].nh;
        // (To look at: the truth and the three, for the farthest views and the middle one.)
        if (!outdir.empty() && (v == (std::max)(0, ia - reach) || v == (ia + ib) / 2 || v == (std::min)(N - 1, ib + reach)))
        {
            sheet.resize((size_t)sheetW * vh * (sheetRows + 1) * 4, 0);
            Put(sheet, sheetW, truth, 0, vh * sheetRows); Put(sheet, sheetW, o1, vw, vh * sheetRows);
            Put(sheet, sheetW, o2, vw * 2, vh * sheetRows); Put(sheet, sheetW, o3, vw * 3, vh * sheetRows);
            ++sheetRows;
        }
    }
    wprintf(L"\n%-52s %-12s %-18s %-14s\n", L"mean PSNR (dB)", L"between", L"up to half outside", L"further out");
    for (int k = 0; k < 3; ++k)
        wprintf(L"%-52s %-12.2f %-18.2f %-14.2f\n", names[k], acc[k].nin ? acc[k].mid / acc[k].nin : 0.0, acc[k].nnear ? acc[k].edge / acc[k].nnear : 0.0, acc[k].nfar ? acc[k].out / acc[k].nfar : 0.0);
    wprintf(L"holes before filling: two-view warp %.2f%%, splats %.2f%% (mean over the targets)\n", acc[1].nh ? acc[1].holes / acc[1].nh : 0.0, acc[2].nh ? acc[2].holes / acc[2].nh : 0.0);
    if (!outdir.empty() && sheetRows)
    {
        const std::wstring p = outdir + L"\\sheet.png";
        SavePNG(p, sheet, (UINT)sheetW, (UINT)(vh * sheetRows));
        wprintf(L"written: %s (columns: the real view, 1, 2, 3; rows: farthest left, middle, farthest right)\n", p.c_str());
    }
    return 0;
}
