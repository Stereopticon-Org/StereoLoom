// Look.hlsl -- look-around: new views of a scene from two real ones.
//
// What goes in is the converter's output: the two eyes side by side, whatever
// layout they came in (the left eye is the left half). What comes out is the
// same, as seen from somewhere else: the left eye from position g_tL, the right
// from g_tR, where the left eye's own place is 0 and the right's is 1.
//
// How (tools/splatlab is the same thing on the CPU, with the reasons measured):
//   1. The two eyes at 1/8, 1/16 and 1/32 of their size, colour on a perceptual
//      scale with the brightness slope along the row beside it (PSDownFirst,
//      PSDownNext, PSGrad).
//   2. Disparity for each eye, coarse to fine: every disparity in range tried at
//      1/32 (PSMatchFull), then a little either side of what the size below and
//      its neighbours found (PSMatchRefine). After each size the two eyes' maps
//      are checked against each other (PSCheck), what fails is given the farther
//      of its row neighbours (PSFill), single wrong texels taken out (PSMedian).
//   3. The geometry thrown forward: every texel of both eyes drawn as a small
//      quad where it is seen from the new place, carrying only its disparity,
//      the nearest winning (VSSplat / PSSplat, a depth buffer). Gaps -- what
//      neither eye saw -- take the farther side (PSHole).
//   4. Each pixel of the new view, at full size, looks its colour up in the real
//      eyes through that disparity (PSCompose). An eye is used only where its
//      own disparity map agrees the surface is there; round a depth edge the
//      candidate on which both eyes agree best is taken. What neither eye sees
//      takes the colour of the nearest pixel that was seen, on the farther side
//      (PSUnseen).
//
// A point seen at x in the left eye is at x - d in the right: d is its
// disparity, in texels of the level, either sign. From position t it is at
// x - t d. Larger d is nearer.

Texture2D    tex0 : register(t0);
Texture2D    tex1 : register(t1);
Texture2D    tex2 : register(t2);
SamplerState sampLinear : register(s0);

cbuffer Params : register(b0)
{
    float2 g_size;      // this pass's target: width of ONE eye, height (texels)
    float2 g_srcSize;   // tex0: width of one eye, height
    float  g_range;     // the largest disparity either way, in this level's texels
    float  g_tL;        // the left eye's new place
    float  g_tR;        // the right eye's
    float  g_eye;       // VSSplat: which new view is being drawn (0 left, 1 right)
    float  g_winR;      // the match window's radius
    float  g_pad;
    float2 g_scale;     // full size over the small maps' size, across and down
};

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSFull(uint id : SV_VertexID)
{
    VSOut o;
    const float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// The pixel's eye (0 left half, 1 right half) and its place within the eye.
void Place(float4 pos, out int half, out int2 p)
{
    const int w = (int)g_size.x;
    half = (int)pos.x >= w ? 1 : 0;
    p = int2((int)pos.x - half * w, (int)pos.y);
}
int2 InEye(int2 p, int half, int w, int h) { return int2(half * w + clamp(p.x, 0, w - 1), clamp(p.y, 0, h - 1)); }

// ---- 1. The levels ----------------------------------------------------------
// The first: an eye at 1/8 -- 64 pixels a texel, read as 16 samples each the
// mean of four. The colour's square root: differences count as the eye sees
// them, dark and light alike.
float4 PSDownFirst(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const float2 srcEye = g_srcSize;
    const float2 k = srcEye / g_size;                       // (source pixels a texel: 8)
    float3 s = 0;
    [unroll] for (int y = 0; y < 4; ++y)
    [unroll] for (int x = 0; x < 4; ++x)
    {
        const float2 q = (float2(p) + (float2(x, y) + 0.5) / 4.0) * k;
        const float2 uv = float2((half * srcEye.x + clamp(q.x, 0.5, srcEye.x - 0.5)) / (2.0 * srcEye.x), q.y / srcEye.y);
        s += tex0.SampleLevel(sampLinear, uv, 0).rgb;
    }
    return float4(sqrt(max(s / 16.0, 0.0)), 0);
}
// The next: half the size below.
float4 PSDownNext(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const int w = (int)g_srcSize.x, h = (int)g_srcSize.y;
    float3 s = 0;
    [unroll] for (int y = 0; y < 2; ++y)
    [unroll] for (int x = 0; x < 2; ++x) s += tex0.Load(int3(InEye(p * 2 + int2(x, y), half, w, h), 0)).rgb;
    return float4(s * 0.25, 0);
}
// ... and the slope of its brightness along the row, in alpha.
float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float4 PSGrad(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const int w = (int)g_size.x, h = (int)g_size.y;
    const float3 c = tex0.Load(int3(InEye(p, half, w, h), 0)).rgb;
    const float g = Luma(tex0.Load(int3(InEye(p + int2(1, 0), half, w, h), 0)).rgb) - Luma(tex0.Load(int3(InEye(p - int2(1, 0), half, w, h), 0)).rgb);
    return float4(c, g);
}

// ---- 2. Disparity -----------------------------------------------------------
// What a candidate costs: the colour difference and the slope difference over
// the window, each capped -- one bad texel cannot decide it. (tex0: the level.)
static const float kCapA = 30.0 / 255.0, kCapG = 20.0 / 255.0, kOut = 14.0 / 255.0;
float Cost(int2 p, int half, int d, int w, int h)
{
    const int R = (int)g_winR;
    const int sgn = half == 0 ? 1 : -1;          // (the left eye's texel is at x - d in the right; the right's at x + d in the left)
    float s = 0;
    [loop] for (int j = -R; j <= R; ++j)
    [loop] for (int k = -R; k <= R; ++k)
    {
        const int xa = clamp(p.x + k, 0, w - 1), ya = clamp(p.y + j, 0, h - 1);
        const int xb = xa - sgn * d;
        if (xb < 0 || xb >= w) { s += kOut; continue; }
        const float4 a = tex0.Load(int3(half * w + xa, ya, 0));
        const float4 b = tex0.Load(int3((1 - half) * w + xb, ya, 0));
        s += 0.35 * min(dot(abs(a.rgb - b.rgb), 1.0 / 3.0), kCapA) + 0.65 * min(abs(a.a - b.a), kCapG);
    }
    return s;
}
// Between candidates: a parabola through the best and its two neighbours. conf:
// how much there was to match on -- the cost's rise either side of the best.
float4 Settle(int2 p, int half, int bd, float best, int w, int h)
{
    const float cm = Cost(p, half, bd - 1, w, h), cp = Cost(p, half, bd + 1, w, h);
    const float den = cm - 2.0 * best + cp;
    const float sub = den > 1e-6 ? clamp(0.5 * (cm - cp) / den, -0.5, 0.5) : 0.0;
    const float n = (2.0 * g_winR + 1.0) * (2.0 * g_winR + 1.0);
    return float4((float)bd + sub, saturate(den / (n * 2.0 / 255.0)), 0, 0);
}
// The smallest level: every disparity in range.
float4 PSMatchFull(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const int w = (int)g_size.x, h = (int)g_size.y, r = (int)g_range;
    float best = 1e30; int bd = 0;
    [loop] for (int d = -r; d <= r; ++d)
    {
        const float c = Cost(p, half, d, w, h);
        if (c < best) { best = c; bd = d; }
    }
    return Settle(p, half, bd, best, w, h);
}
// The others: a little either side of the level below's answer, and its
// neighbours' answers as they are -- a thin thing lost below is found again
// from beside it. (tex1: the level below's disparity, half this size.)
float4 PSMatchRefine(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const int w = (int)g_size.x, h = (int)g_size.y, r = (int)g_range;
    const int lw = w / 2, lh = h / 2;
    float best = 1e30; int bd = 0;
    static const int2 kN[5] = { int2(0, 0), int2(-3, 0), int2(3, 0), int2(0, -3), int2(0, 3) };
    [loop] for (int n = 0; n < 5; ++n)
    {
        const int2 q = clamp(p + kN[n], 0, int2(w - 1, h - 1));
        const int c0 = (int)round(2.0 * tex1.Load(int3(InEye(q / 2, half, lw, lh), 0)).r);
        const int reach = n == 0 ? 2 : 0;
        [loop] for (int k = -reach; k <= reach; ++k)
        {
            const int d = clamp(c0 + k, -r, r);
            const float c = Cost(p, half, d, w, h);
            if (c < best) { best = c; bd = d; }
        }
    }
    return Settle(p, half, bd, best, w, h);
}

// The other eye's disparity at a place between its texels. (tex: .r)
float DispAt(Texture2D t, float x, int y, int half, int w, int h)
{
    const float xc = clamp(x, 0.0, (float)w - 1.0);
    const int x0 = (int)floor(xc), x1 = min(x0 + 1, w - 1);
    const float f = xc - (float)x0;
    const int yc = clamp(y, 0, h - 1);
    return lerp(t.Load(int3(half * w + x0, yc, 0)).r, t.Load(int3(half * w + x1, yc, 0)).r, f);
}
// The two eyes' maps checked against each other: a texel at d must land on one
// of the other eye's that points back, within a texel. .b: 1 passed. (tex0: the disparity.)
float4 PSCheck(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const int w = (int)g_size.x, h = (int)g_size.y;
    const float4 v = tex0.Load(int3(half * w + p.x, p.y, 0));
    const float sgn = half == 0 ? 1.0 : -1.0;
    const float xb = (float)p.x - sgn * v.r;
    const bool ok = xb >= 0.0 && xb <= (float)w - 1.0 && abs(DispAt(tex0, xb, p.y, 1 - half, w, h) - v.r) < 1.0;
    return float4(v.r, ok ? v.g : 0.0, ok ? 1.0 : 0.0, 0);
}
// What failed is hidden from the other eye -- background: the FARTHER of the
// nearest texels either side along the row that passed. (tex0: PSCheck's.)
float4 PSFill(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const int w = (int)g_size.x, h = (int)g_size.y;
    const float4 v = tex0.Load(int3(half * w + p.x, p.y, 0));
    if (v.b > 0.5) return v;
    float d = v.r; bool have = false;
    [loop] for (int l = 1; l <= 48 && p.x - l >= 0; ++l)
    {
        const float4 q = tex0.Load(int3(half * w + p.x - l, p.y, 0));
        if (q.b > 0.5) { d = q.r; have = true; break; }
    }
    [loop] for (int r = 1; r <= 48 && p.x + r < w; ++r)
    {
        const float4 q = tex0.Load(int3(half * w + p.x + r, p.y, 0));
        if (q.b > 0.5) { d = have ? min(d, q.r) : q.r; break; }
    }
    return float4(d, 0, 0, 0);
}
// A 3x3 median of the disparity: single wrong texels out, edges kept. (tex0.)
void Sort2(inout float a, inout float b) { const float lo = min(a, b), hi = max(a, b); a = lo; b = hi; }
float4 PSMedian(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const int w = (int)g_size.x, h = (int)g_size.y;
    float v[9];
    [unroll] for (int j = 0; j < 3; ++j)
    [unroll] for (int k = 0; k < 3; ++k) v[j * 3 + k] = tex0.Load(int3(InEye(p + int2(k - 1, j - 1), half, w, h), 0)).r;
    // (The middle of nine by a fixed set of swaps.)
    Sort2(v[1], v[2]); Sort2(v[4], v[5]); Sort2(v[7], v[8]); Sort2(v[0], v[1]); Sort2(v[3], v[4]); Sort2(v[6], v[7]);
    Sort2(v[1], v[2]); Sort2(v[4], v[5]); Sort2(v[7], v[8]); Sort2(v[0], v[3]); Sort2(v[5], v[8]); Sort2(v[4], v[7]);
    Sort2(v[3], v[6]); Sort2(v[1], v[4]); Sort2(v[2], v[5]); Sort2(v[4], v[7]); Sort2(v[4], v[2]); Sort2(v[6], v[4]);
    Sort2(v[4], v[2]);
    const float4 c = tex0.Load(int3(half * w + p.x, p.y, 0));
    return float4(v[4], c.g, c.b, 0);
}

// ---- 3. The geometry thrown forward -----------------------------------------
// Six vertices a texel, both eyes' texels one after the other: a quad from the
// texel's left edge to its right, each edge moved by its own disparity where
// the neighbour is the same surface -- a slanted wall stays closed, a depth
// edge opens. Depth from the disparity: the nearest wins. (tex0: the disparity
// at 1/8; drawn once for each new view, into that view's half.)
struct SplatOut { float4 pos : SV_POSITION; nointerpolation float d : TEXCOORD0; };
SplatOut VSSplat(uint id : SV_VertexID)
{
    const int w = (int)g_size.x, h = (int)g_size.y;
    const uint quad = id / 6, corner = id % 6;
    const int ref = (int)(quad / (uint)(w * h));
    const int idx = (int)(quad % (uint)(w * h));
    const int x = idx % w, y = idx / w;
    const float t = g_eye < 0.5 ? g_tL : g_tR;
    const float s = (float)ref - t;                       // (the left eye's texels go by -t d, the right's by (1 - t) d)
    const float d  = tex0.Load(int3(ref * w + x, y, 0)).r;
    const float dl = tex0.Load(int3(ref * w + max(x - 1, 0), y, 0)).r;
    const float dr = tex0.Load(int3(ref * w + min(x + 1, w - 1), y, 0)).r;
    const float gl = abs(d - dl) < 1.0 ? 0.5 * (d + dl) : d, gr = abs(dr - d) < 1.0 ? 0.5 * (d + dr) : d;
    float x0 = (float)x + s * gl, x1 = (float)x + 1.0 + s * gr;
    if (x1 < x0) { const float tmp = x0; x0 = x1; x1 = tmp; }
    // (Corners: 0 1 2, 2 1 3 of the quad.)
    static const uint kC[6] = { 0, 1, 2, 2, 1, 3 };
    const uint c = kC[corner];
    const float px = (c & 1) ? x1 : x0, py = (float)y + ((c & 2) ? 1.0 : 0.0);
    SplatOut o;
    o.pos = float4(px / (float)w * 2.0 - 1.0, 1.0 - py / (float)h * 2.0, saturate(0.5 - d / (4.0 * max(g_range, 1.0))), 1.0);
    o.d = d;
    return o;
}
float4 PSSplat(SplatOut i) : SV_Target { return float4(i.d, 1, 0, 0); }

// Where nothing landed (.g 0: cleared), neither eye saw: the FARTHER of the
// nearest texels either side along the row. .g: 1 a texel something landed on.
// (tex0: the new views' disparity, both side by side.)
float4 PSHole(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const int w = (int)g_size.x;
    const float2 v = tex0.Load(int3(half * w + p.x, p.y, 0)).rg;
    if (v.g > 0.5) return float4(v.r, 1, 0, 0);
    float d = 0.0; bool have = false;
    [loop] for (int l = 1; l <= 64 && p.x - l >= 0; ++l)
    {
        const float2 q = tex0.Load(int3(half * w + p.x - l, p.y, 0)).rg;
        if (q.g > 0.5) { d = q.r; have = true; break; }
    }
    [loop] for (int r = 1; r <= 64 && p.x + r < w; ++r)
    {
        const float2 q = tex0.Load(int3(half * w + p.x + r, p.y, 0)).rg;
        if (q.g > 0.5) { d = have ? min(d, q.r) : q.r; break; }
    }
    return float4(d, 0, 0, 0);
}

// ---- 4. The new views -------------------------------------------------------
// tex0: the two real eyes, full size. tex1: their disparity at 1/8 (g_srcSize
// is its eye's size). tex2: the new views' disparity at 1/8.
// One candidate disparity tried: each eye's colour through it, whether each eye
// sees it, and what it costs -- how far the eyes that see it differ.
void Try(float d, float2 lo, float2 full, float t, int w, int h, out float3 col, out float cost, out float seen)
{
    float3 c[2]; float vis[2]; float wg[2];
    [unroll] for (int r = 0; r < 2; ++r)
    {
        const float s = (float)r - t;
        const float xs = lo.x - s * d;                                   // (the small maps' place in that eye)
        const float xf = clamp(full.x - s * d * g_scale.x, 0.5, g_size.x - 0.5);
        c[r] = tex0.SampleLevel(sampLinear, float2(((float)r * g_size.x + xf) / (2.0 * g_size.x), full.y / g_size.y), 0).rgb;
        vis[r] = (xs >= 0.0 && xs <= (float)w - 1.0 && abs(DispAt(tex1, xs, (int)(lo.y + 0.5), r, w, h) - d) < 1.0) ? 1.0 : 0.0;
        wg[r] = max(0.001, 1.0 - abs(t - (float)r));   // (at an eye's own place, that eye alone: the picture as it went in)
    }
    const float nv = vis[0] + vis[1];
    cost = nv < 0.5 ? 60.0 / 255.0 : 12.0 / 255.0;
    if (nv > 1.5) cost = dot(abs(sqrt(max(c[0], 0.0)) - sqrt(max(c[1], 0.0))), 1.0 / 6.0);
    // (Neither sees it: the eye nearest the new place, as it is -- a guess, filled by PSUnseen.)
    const float w0 = nv > 0.5 ? vis[0] * wg[0] : wg[0] * wg[0], w1 = nv > 0.5 ? vis[1] * wg[1] : wg[1] * wg[1];
    col = (c[0] * w0 + c[1] * w1) / max(w0 + w1, 1e-6);
    seen = nv > 0.5 ? 1.0 : 0.0;
}
float4 PSCompose(VSOut i) : SV_Target
{
    const int W = (int)g_size.x;
    const int pane = (int)i.pos.x >= W ? 1 : 0;
    const float2 full = float2(i.pos.x - (float)(pane * W), i.pos.y);    // (pixel centre within the eye)
    const float t = pane == 0 ? g_tL : g_tR;
    // (At an eye's own place the new view is that eye, pixel for pixel: nothing
    // to work out, and nothing that could differ.)
    if (abs(t - round(t)) < 1e-4 && t > -0.5 && t < 1.5)
        return float4(tex0.Load(int3((int)round(t) * W + (int)full.x, (int)full.y, 0)).rgb, 1);
    const int w = (int)g_srcSize.x, h = (int)g_srcSize.y;
    const float2 lo = full / g_scale - 0.5;
    const int x0 = clamp((int)floor(lo.x), 0, w - 1), y0 = clamp((int)floor(lo.y), 0, h - 1);
    const int x1 = min(x0 + 1, w - 1), y1 = min(y0 + 1, h - 1);
    const float2 f = saturate(lo - float2(x0, y0));
    const float d00 = tex2.Load(int3(pane * w + x0, y0, 0)).r, d10 = tex2.Load(int3(pane * w + x1, y0, 0)).r;
    const float d01 = tex2.Load(int3(pane * w + x0, y1, 0)).r, d11 = tex2.Load(int3(pane * w + x1, y1, 0)).r;
    const float dBi = lerp(lerp(d00, d10, f.x), lerp(d01, d11, f.x), f.y);
    float3 col; float cost, seen;
    Try(dBi, lo, full, t, w, h, col, cost, seen);
    // (The four round it agree: one surface, nothing to choose between -- most
    // of the picture. Else each of the four is tried, and the in-between value
    // kept only where it is clearly no worse.)
    [branch] if (max(max(d00, d10), max(d01, d11)) - min(min(d00, d10), min(d01, d11)) > 0.25)
    {
        cost += 0.5 / 255.0;
        const float cand[4] = { d00, d10, d01, d11 };
        [unroll] for (int k = 0; k < 4; ++k)
        {
            float3 c2; float cost2, seen2;
            Try(cand[k], lo, full, t, w, h, c2, cost2, seen2);
            if (cost2 < cost) { cost = cost2; col = c2; seen = seen2; }
        }
    }
    return float4(col, seen);
}
// What neither eye sees is behind something: the colour of the nearest pixel
// that was seen, on the FARTHER side along the row. (tex0: PSCompose's, .a 1
// seen. tex2: the new views' disparity. g_size: the full eye.)
float4 PSUnseen(VSOut i) : SV_Target
{
    const int W = (int)g_size.x;
    const int pane = (int)i.pos.x >= W ? 1 : 0;
    const int2 p = int2((int)i.pos.x - pane * W, (int)i.pos.y);
    const float4 v = tex0.Load(int3(pane * W + p.x, p.y, 0));
    if (v.a > 0.5) return float4(v.rgb, 1);
    const int w = (int)g_srcSize.x, h = (int)g_srcSize.y;
    const int ly = clamp((int)((float)p.y / g_scale.y), 0, h - 1);
    float3 cl = v.rgb, cr = v.rgb; float dl = 1e30, dr = 1e30;
    [loop] for (int l = 1; l <= 256 && p.x - l >= 0; ++l)
    {
        const float4 q = tex0.Load(int3(pane * W + p.x - l, p.y, 0));
        if (q.a > 0.5) { cl = q.rgb; dl = tex2.Load(int3(pane * w + clamp((int)((float)(p.x - l) / g_scale.x), 0, w - 1), ly, 0)).r; break; }
    }
    [loop] for (int r = 1; r <= 256 && p.x + r < W; ++r)
    {
        const float4 q = tex0.Load(int3(pane * W + p.x + r, p.y, 0));
        if (q.a > 0.5) { cr = q.rgb; dr = tex2.Load(int3(pane * w + clamp((int)((float)(p.x + r) / g_scale.x), 0, w - 1), ly, 0)).r; break; }
    }
    if (dl > 1e29 && dr > 1e29) return float4(v.rgb, 1);
    return float4(dl < dr ? cl : cr, 1);
}
