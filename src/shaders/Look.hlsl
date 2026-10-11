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
Texture2D    tex3 : register(t3);
Texture2D    tex4 : register(t4);
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
    float  g_ty;        // the new place's height: eye spacings DOWN the screen from the eyes' own
    float2 g_scale;     // full size over the small maps' size, across and down
    float  g_rho;       // the eyes' own distance from the screen over the new place's (1: as far; 2: half as far)
    float  g_kx;        // (g_rho - 1) x the screen's width in eye spacings: see Move
    float  g_ky;        // ... and its height
    float  g_only;      // a test: -1 both eyes are used; 0 / 1 that eye alone
    float4 g_opt;       // switches under trial: .x the reach, in texels, that PSCompose looks for other surfaces; .y what a surface the maps call hidden must beat (0: it is not tried); .z how softly surfaces mix; .w spare
    float4 g_motion;    // how the picture moved since the one before: .xy everything alike, in its own pixels; .zw times its disparity more (a camera a tenth of an eye spacing to the side: 0.1)
    float4 g_plate;     // .x 1: there is a plate from before (PSPlate). .y 1: PSUnseen may fill from the plate. .z a test: what it fills is marked. .w 1: there is a painted background (tex4)
    float4 g_pad;       // .xy: the plate's margin round the eye's picture, in pixels -- what has gone out of the picture's side is kept a while too. .z: how many levels the far-side fill has (tex3, PSFillDown); 0: none
    float4 g_lk;        // the motion finder: .x picture pixels a texel of this level; .y the disparity (picture pixels) counted as one; .z this level's disparity texels to picture pixels; .w 1: PSPlate takes the motion found (tex3)
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

// How far a point of eye r moves on the way to the view from t, in disparities
// (across, down): it is drawn at  place - d * Move.  The display is a window:
// what lies on the screen (d = 0) stays put whatever the head does. Sideways
// and up-and-down the move is the head's own, in eye spacings. Nearer the
// screen (g_rho > 1) everything behind it spreads out from the point in front
// of the head and everything before it draws in -- g_kx / g_ky, with where the
// point is on the screen (uv, 0..1 across the eye's picture).
float2 Move(float r, float t, float2 uv)
{
    return float2((t - 0.5) * g_rho - (r - 0.5) - g_kx * (uv.x - 0.5), g_ty * g_rho - g_ky * (uv.y - 0.5));
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
// (Tried, 2026-10-06: each texel of the window counted by how like the middle
// one it is in colour, to keep a window to its own side of an edge. Measured
// worse on every test -- quilts by 1 to 2 dB, a game frame slightly -- so every
// texel counts alike.)
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
        const float4 a = tex0.Load(int3(half * w + xa, ya, 0));
        if (xb < 0 || xb >= w) { s += kOut; continue; }
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
    const int R = (int)g_winR;
    const int sgn = half == 0 ? 1 : -1;
    // Its own start and two either side: five candidates a texel apart, so the
    // window is read once for all five -- its own eye's texel once, the other
    // eye's five in a row. (Each on its own read both again: twice the reads.)
    const int c0 = clamp((int)round(2.0 * tex1.Load(int3(InEye(p / 2, half, lw, lh), 0)).r), -r + 2, r - 2);
    float c5[5] = { 0, 0, 0, 0, 0 };
    [loop] for (int j = -R; j <= R; ++j)
    [loop] for (int k = -R; k <= R; ++k)
    {
        const int xa = clamp(p.x + k, 0, w - 1), ya = clamp(p.y + j, 0, h - 1);
        const float4 a = tex0.Load(int3(half * w + xa, ya, 0));
        [unroll] for (int m = 0; m < 5; ++m)
        {
            const int xb = xa - sgn * (c0 + m - 2);
            if (xb < 0 || xb >= w) { c5[m] += kOut; continue; }
            const float4 b = tex0.Load(int3((1 - half) * w + xb, ya, 0));
            c5[m] += 0.35 * min(dot(abs(a.rgb - b.rgb), 1.0 / 3.0), kCapA) + 0.65 * min(abs(a.a - b.a), kCapG);
        }
    }
    float best = c5[0]; int bm = 0;
    [unroll] for (int m2 = 1; m2 < 5; ++m2) if (c5[m2] < best) { best = c5[m2]; bm = m2; }
    int bd = c0 + bm - 2; bool five = true;
    // Its neighbours' starts, as they are, where they are not among those five.
    static const int2 kN[4] = { int2(-3, 0), int2(3, 0), int2(0, -3), int2(0, 3) };
    [loop] for (int n = 0; n < 4; ++n)
    {
        const int2 q = clamp(p + kN[n], 0, int2(w - 1, h - 1));
        const int cn = clamp((int)round(2.0 * tex1.Load(int3(InEye(q / 2, half, lw, lh), 0)).r), -r, r);
        if (abs(cn - c0) <= 2) continue;
        const float c = Cost(p, half, cn, w, h);
        if (c < best) { best = c; bd = cn; five = false; }
    }
    // (Between candidates: where the best is one of the five with one each
    // side, the parabola is through costs already here.)
    if (five && bm > 0 && bm < 4)
    {
        float cm = 0, cp = 0;
        [unroll] for (int m3 = 0; m3 < 5; ++m3) { cm = (m3 == bm - 1) ? c5[m3] : cm; cp = (m3 == bm + 1) ? c5[m3] : cp; }
        const float den = cm - 2.0 * best + cp;
        const float sub = den > 1e-6 ? clamp(0.5 * (cm - cp) / den, -0.5, 0.5) : 0.0;
        const float nw = (2.0 * g_winR + 1.0) * (2.0 * g_winR + 1.0);
        return float4((float)bd + sub, saturate(den / (nw * 2.0 / 255.0)), 0, 0);
    }
    return Settle(p, half, bd, best, w, h);
}

// Edges drawn in: the windows above are several texels across, so an object's
// disparity spreads that far past its outline (what shows as a halo, and a
// flat sign cut out too generously), and a small thing can be outvoted by
// what is behind it. Here each texel asks again, with a window of 3x3 only,
// whether one of the disparities found a few texels away suits it better than
// its own -- the choice is only among answers already found nearby, so a small
// window's noise cannot invent one. Its own stands unless another is clearly
// better. Where all eight neighbours looked at have its own disparity -- the
// inside of a surface, most of the picture -- there is nothing to ask.
// (tex0: the level. tex1: the disparity. g_winR: 1.)
float4 PSSpread(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const int w = (int)g_size.x, h = (int)g_size.y;
    const float4 own = tex1.Load(int3(half * w + p.x, p.y, 0));
    static const int2 kS[8] = { int2(-3, 0), int2(3, 0), int2(0, -3), int2(0, 3), int2(-6, 0), int2(6, 0), int2(0, -6), int2(0, 6) };
    float cd[8]; bool any = false;
    [unroll] for (int n = 0; n < 8; ++n)
    {
        cd[n] = tex1.Load(int3(InEye(p + kS[n], half, w, h), 0)).r;
        any = any || abs(cd[n] - own.r) >= 0.75;
    }
    [branch] if (!any) return own;
    float bestD = own.r;
    float best = Cost(p, half, (int)round(own.r), w, h) * 0.85;
    [unroll] for (int n2 = 0; n2 < 8; ++n2)
    {
        // (One that differs from its own, and from those already tried.)
        bool fresh = abs(cd[n2] - own.r) >= 0.75;
        [unroll] for (int m = 0; m < n2; ++m) fresh = fresh && abs(cd[n2] - cd[m]) >= 0.5;
        [branch] if (fresh)
        {
            const float c = Cost(p, half, (int)round(cd[n2]), w, h);
            if (c < best) { best = c; bestD = cd[n2]; }
        }
    }
    return float4(bestD, own.g, own.b, 0);
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
// Six vertices a texel, both eyes' texels one after the other: a quad whose
// four corners are each moved by the disparity of the surface at that corner --
// the mean of the texels round it that are the same surface as this one. So
// neighbouring quads share their corners along a wall, however it slants, and
// part at a depth edge. Depth from the texel's own disparity: the nearest wins.
// (tex0: the disparity at the small size; drawn once for each new view, into
// that view's half.)
struct SplatOut { float4 pos : SV_POSITION; nointerpolation float d : TEXCOORD0; };
SplatOut VSSplat(uint id : SV_VertexID)
{
    const int w = (int)g_size.x, h = (int)g_size.y;
    const uint quad = id / 6u, corner = id % 6u;
    const uint per = (uint)(w * h);
    const int ref = (int)(quad / per);
    const uint idx = quad % per;
    const int x = (int)(idx % (uint)w), y = (int)(idx / (uint)w);
    const float t = g_eye < 0.5 ? g_tL : g_tR;
    const float d = tex0.Load(int3(ref * w + x, y, 0)).r;
    // (Corners: 0 1 2, 2 1 3 of the quad.)
    static const uint kC[6] = { 0, 1, 2, 2, 1, 3 };
    const uint c = kC[corner];
    const int cx = (int)(c & 1u), cy = (int)((c >> 1) & 1u);
    float sum = 0.0, n = 0.0;
    [unroll] for (int j = 0; j < 2; ++j)
    [unroll] for (int k = 0; k < 2; ++k)
    {
        const float q = tex0.Load(int3(ref * w + clamp(x - 1 + cx + k, 0, w - 1), clamp(y - 1 + cy + j, 0, h - 1), 0)).r;
        if (abs(q - d) < 1.0) { sum += q; n += 1.0; }
    }
    const float dc = n > 0.5 ? sum / n : d;
    const float2 at = float2((float)(x + cx), (float)(y + cy));
    const float2 p = at - dc * Move((float)ref, t, at / float2(w, h));
    SplatOut o;
    o.pos = float4(p.x / (float)w * 2.0 - 1.0, 1.0 - p.y / (float)h * 2.0, saturate(0.5 - d / (4.0 * max(g_range, 1.0))), 1.0);
    // (A test with one eye alone: the other's quads are not drawn.)
    if (g_only > -0.5 && g_only < 1.5 && abs(g_only - (float)ref) > 0.5) o.pos = float4(2.0, 2.0, 2.0, 1.0);
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
    [loop] for (int l = 1; l <= 256 && p.x - l >= 0; ++l)
    {
        const float2 q = tex0.Load(int3(half * w + p.x - l, p.y, 0)).rg;
        if (q.g > 0.5) { d = q.r; have = true; break; }
    }
    [loop] for (int r = 1; r <= 256 && p.x + r < w; ++r)
    {
        const float2 q = tex0.Load(int3(half * w + p.x + r, p.y, 0)).rg;
        if (q.g > 0.5) { d = have ? min(d, q.r) : q.r; break; }
    }
    return float4(d, 0, 0, 0);
}

// ---- 4. The new views -------------------------------------------------------
// tex0: the two real eyes, full size. tex1: their disparity at the small size
// (g_srcSize is its eye's size). tex2: the new views' disparity, small.
// One candidate disparity tried: each eye's colour through it, whether each eye
// sees it, and what it costs -- how far the eyes that see it differ.
// (Set by PSCompose: nothing of either eye landed anywhere round this pixel -- a gap. There the
// eyes agreeing is chance, and is not believed.)
static bool s_gap = false;
// (Tried, 2026-10-12, and taken out: the eye further from the new place not read at all where it counts for
// next to nothing and the nearer sees the surface. Half the lookups for a view outside the eyes -- and the
// pass was slower for it, 1.61 ms against 1.41: the branch cost more than the reads it saved.)
void Try(float d, float2 lo, float2 full, float t, int w, int h, out float3 col, out float cost, out float seen)
{
    float3 c[2]; float vis[2]; float wg[2];
    const float2 uv = full / g_size;
    [unroll] for (int r = 0; r < 2; ++r)
    {
        const float2 mv = d * Move((float)r, t, uv);
        const float2 ps = lo + mv;                                        // (the small maps' place in that eye)
        const float2 pf = clamp(full + mv * g_scale, 0.5, g_size - 0.5);
        c[r] = tex0.SampleLevel(sampLinear, float2(((float)r * g_size.x + pf.x) / (2.0 * g_size.x), pf.y / g_size.y), 0).rgb;
        const float2 pd = clamp(ps + 0.5, 0.5, float2(w, h) - 0.5);
        const float dr = tex1.SampleLevel(sampLinear, float2(((float)(r * w) + pd.x) / (float)(2 * w), pd.y / (float)h), 0).r;
        const bool inside = ps.x >= 0.0 && ps.x <= (float)w - 1.0 && ps.y >= 0.0 && ps.y <= (float)h - 1.0;
        vis[r] = (inside && abs(dr - d) < 1.0) ? 1.0 : 0.0;
        // (How much each eye counts.
        // Between the eyes, by nearness, as two exposures mixed; beyond them the
        // nearer one, however far out the new place is.)
        const float away = abs(t - (float)r);
        wg[r] = max(1.0 - away, 0.02 * exp(-2.0 * away));
        if (g_only > -0.5 && g_only < 1.5 && abs(g_only - (float)r) > 0.5) { vis[r] = 0.0; wg[r] = 0.0; }
    }
    const float nv = vis[0] + vis[1];
    cost = nv < 0.5 ? 60.0 / 255.0 : 12.0 / 255.0;
    const float both = dot(abs(sqrt(max(c[0], 0.0)) - sqrt(max(c[1], 0.0))), 1.0 / 6.0);
    if (nv > 1.5) cost = both;
    // (The maps are a few texels fat at an outline: what they call hidden there
    // may be in plain sight of both eyes. Where the two eyes' colours agree
    // through this disparity better than the maps' word is worth, they are
    // believed.)
    bool agreed = false;
    [branch] if (nv < 1.5 && g_opt.y > 0.0 && g_only < -0.5 && !s_gap && both + g_opt.y < cost)
    {
        // (... but only at an outline: where an eye's map has this surface within
        // a few texels of the place. Further in, hidden is hidden -- two places
        // on whatever hides it can look alike too.)
        bool nearAll = true;
        const int R = (int)g_opt.w;
        [unroll] for (int e = 0; e < 2; ++e)
        {
            [branch] if (vis[e] < 0.5)
            {
                const float2 ps = lo + d * Move((float)e, t, uv);
                const int2 pi = int2((int)floor(ps.x + 0.5), (int)floor(ps.y + 0.5));
                bool nr = false;
                static const int2 kO[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
                [unroll] for (int n = 0; n < 4; ++n)
                {
                    const int2 q = pi + kO[n] * R;
                    const bool in2 = q.x >= 0 && q.x < w && q.y >= 0 && q.y < h;
                    nr = nr || (in2 && abs(tex1.Load(int3(e * w + q.x, q.y, 0)).r - d) < 1.0);
                }
                nearAll = nearAll && nr;
            }
        }
        if (nearAll || R <= 0) { cost = both + g_opt.y; agreed = true; }
    }
    // (Neither sees it: the eye nearest the new place, as it is -- a guess, filled by PSUnseen.)
    const float w0 = agreed ? wg[0] : nv > 0.5 ? vis[0] * wg[0] : wg[0] * wg[0], w1 = agreed ? wg[1] : nv > 0.5 ? vis[1] * wg[1] : wg[1] * wg[1];
    col = (c[0] * w0 + c[1] * w1) / max(w0 + w1, 1e-12);
    seen = (nv > 1.5 || agreed) ? 2.0 : nv > 0.5 ? 1.0 : 0.0;   // (2: both eyes vouch for it)
}
float4 PSCompose(VSOut i) : SV_Target
{
    const int W = (int)g_size.x;
    const int pane = (int)i.pos.x >= W ? 1 : 0;
    const float2 full = float2(i.pos.x - (float)(pane * W), i.pos.y);    // (pixel centre within the eye)
    const float t = pane == 0 ? g_tL : g_tR;
    // (At an eye's own place the new view is that eye, pixel for pixel: nothing
    // to work out, and nothing that could differ.)
    if (abs(t - round(t)) < 1e-4 && t > -0.5 && t < 1.5 && abs(g_ty) < 1e-4 && abs(g_rho - 1.0) < 1e-4 && g_only < -0.5)
        return float4(tex0.Load(int3((int)round(t) * W + (int)full.x, (int)full.y, 0)).rgb, 1);
    const int w = (int)g_srcSize.x, h = (int)g_srcSize.y;
    const float2 lo = full / g_scale - 0.5;
    // (The four texels round it in one read. Kept a texel inside the eye's
    // right edge and the bottom, so the four are all this eye's.)
    const int x0 = clamp((int)floor(lo.x), 0, w - 2), y0 = clamp((int)floor(lo.y), 0, h - 2);
    const float2 f = saturate(lo - float2(x0, y0));
    const float4 g4 = tex2.GatherRed(sampLinear, float2((float)(pane * w + x0) + 1.0, (float)y0 + 1.0) / float2(2 * w, h));
    const float4 l4 = tex2.GatherGreen(sampLinear, float2((float)(pane * w + x0) + 1.0, (float)y0 + 1.0) / float2(2 * w, h));
    s_gap = max(max(l4.x, l4.y), max(l4.z, l4.w)) < 0.5;
    const float d00 = g4.w, d10 = g4.z, d01 = g4.x, d11 = g4.y;
    const float dBi = lerp(lerp(d00, d10, f.x), lerp(d01, d11, f.x), f.y);
    // (To look at: the geometry itself -- the new view's disparity as grey, the
    // screen's own depth mid-grey, nearer lighter. g_only 2.)
    if (g_only > 1.5) { const float g = saturate(0.5 + dBi / (0.5 * max(g_range, 1.0))); return float4(g * g, g * g, g * g, 1); }
    float3 col; float cost, seen;
    Try(dBi, lo, full, t, w, h, col, cost, seen);
    // (The four round it agree: one surface, nothing to choose between -- most
    // of the picture. Else each of the four is tried, and the in-between value
    // kept only where it is clearly no worse.)
    // (g_opt.x: the surfaces a few texels off are looked at too -- the maps are
    // that fat at an outline, so the surface a pixel is really on may be one
    // that the four round it do not have. Each pixel of the picture then
    // chooses for itself, at its own size, which of the surfaces near it the
    // two eyes agree on.)
    float far4[4] = { dBi, dBi, dBi, dBi };
    float lo4 = min(min(d00, d10), min(d01, d11)), hi4 = max(max(d00, d10), max(d01, d11));
    const int reach = (int)g_opt.x;
    [branch] if (reach > 0)
    {
        static const int2 kF[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
        [unroll] for (int n = 0; n < 4; ++n)
        {
            const int2 q = clamp(int2(x0, y0) + (kF[n].x + kF[n].y > 0 ? 1 : 0) + kF[n] * reach, 0, int2(w - 1, h - 1));
            far4[n] = tex2.Load(int3(pane * w + q.x, q.y, 0)).r;
            lo4 = min(lo4, far4[n]); hi4 = max(hi4, far4[n]);
        }
    }
    // (g_opt.z under 0, with a trained matcher's depth: the edge strip. The map is a texel fat at an
    // outline -- so within two texels of a step, a pixel the map calls NEAR may be the thing or may be
    // what is behind it, and a pixel on the outline itself is some of each. Which, and how much, is read
    // off the picture: the pixel's colour set between the thing's colour a little way in and the
    // background's a little way out is its share of the thing (a matte). The thing's share is kept, and
    // the background's share is replaced by the background where it now is -- so an outline is soft and
    // lies where the picture has it, not where the map's texels do.)
    [branch] if (g_opt.z < -0.5 && reach > 0)
    {
        const float nearD = hi4, farD = lo4;
        [branch] if (nearD - farD > 1.5 && dBi > 0.5 * (nearD + farD))
        {
            // (Towards the thing: up the map's slope, from the four a little way off.)
            float2 n = float2(far4[1] - far4[0], far4[3] - far4[2]);
            const float nl = length(n);
            [branch] if (nl > 0.5)
            {
                n /= nl;
                const int r = abs(t) <= abs(t - 1.0) ? 0 : 1;                  // (the eye nearer the new place)
                const float2 pS = full + dBi * Move((float)r, t, full / g_size) * g_scale;
                const float2 pFg = pS + n * 2.0 * g_scale, pBg = pS - n * 3.0 * g_scale;
                const float3 C = tex0.SampleLevel(sampLinear, float2(((float)r * g_size.x + clamp(pS.x, 0.5, g_size.x - 0.5)) / (2.0 * g_size.x), pS.y / g_size.y), 0).rgb;
                const float3 F = tex0.SampleLevel(sampLinear, float2(((float)r * g_size.x + clamp(pFg.x, 0.5, g_size.x - 0.5)) / (2.0 * g_size.x), pFg.y / g_size.y), 0).rgb;
                const float3 B = tex0.SampleLevel(sampLinear, float2(((float)r * g_size.x + clamp(pBg.x, 0.5, g_size.x - 0.5)) / (2.0 * g_size.x), pBg.y / g_size.y), 0).rgb;
                const float3 fb = sqrt(max(F, 0.0)) - sqrt(max(B, 0.0));
                const float den = dot(fb, fb);
                // (The two alike in colour: nothing to read. The map's word stands.)
                const float alpha = den > 0.004 ? saturate(dot(sqrt(max(C, 0.0)) - sqrt(max(B, 0.0)), fb) / den) : 1.0;
                [branch] if (alpha < 0.97)
                {
                    float3 cFar; float kFar, sFar;
                    Try(farD, lo, full, t, w, h, cFar, kFar, sFar);
                    // (A plain mix by the share. Taking the old background out of the pixel and putting the new in --
                    // col + (1 - alpha)(cFar - B) -- is the truer sum, and where the share is misjudged it gave
                    // specks of false colour.)
                    const float a2 = smoothstep(0.2, 0.8, alpha);
                    const float3 mixed = lerp(cFar, col, a2);
                    return float4(mixed, (alpha > 0.5 ? seen : sFar) > 0.5 ? 1.0 : 0.0);
                }
            }
        }
    }
    [branch] if (hi4 - lo4 > 0.25)
    {
        cost += 0.5 / 255.0;
        // (Across a real step the value between the four is on neither surface -- a point hanging in the
        // space between a thing and what is behind it. It is not one of the choices there.)
        if (max(max(d00, d10), max(d01, d11)) - min(min(d00, d10), min(d01, d11)) > 1.5) cost = 1e3;
        const float cand[8] = { d00, d10, d01, d11, far4[0], far4[1], far4[2], far4[3] };
        float3 acc = col * 1e-6; float accW = 1e-6, accSeen = 0;
        const float c0 = cost;
        float cmin = cost; float3 cbest = col; float sbest = seen;
        float cs[8]; float3 cc[8]; float ss[8];
        [loop] for (int k = 0; k < (reach > 0 && g_opt.z > -0.5 ? 8 : 4); ++k)
        {
            cs[k] = 1e3; cc[k] = 0; ss[k] = 0;
            // (One the bilinear value or an earlier one already stands for is not tried again.)
            bool fresh = abs(cand[k] - dBi) > 0.2;
            for (int m = 0; m < k; ++m) fresh = fresh && abs(cand[k] - cand[m]) > 0.2;
            [branch] if (fresh)
            {
                Try(cand[k], lo, full, t, w, h, cc[k], cs[k], ss[k]);
                // (One from further off stands only on both eyes' word.)
                if (k >= 4) cs[k] = ss[k] > 1.5 ? cs[k] + 1.0 / 255.0 : 1e3;
                if (cs[k] < cmin) { cmin = cs[k]; cbest = cc[k]; sbest = ss[k]; }
            }
        }
        // (g_opt.z: not one of them but a mix, by how well each does -- a soft edge
        // where two do nearly as well.)
        const float soft = g_opt.z < -0.5 ? 0.01 : g_opt.z;   // (with the edge strip: the rest as softly as before)
        [branch] if (soft > 0.0)
        {
            float wS = exp(-(c0 - cmin) / soft); acc = col * wS; accW = wS; accSeen = min(seen, 1.0) * wS;
            [loop] for (int k2 = 0; k2 < (reach > 0 && g_opt.z > -0.5 ? 8 : 4); ++k2)
            {
                const float wk = cs[k2] < 100.0 ? exp(-(cs[k2] - cmin) / soft) : 0.0;
                acc += cc[k2] * wk; accW += wk; accSeen += min(ss[k2], 1.0) * wk;
            }
            return float4(acc / accW, accSeen / accW > 0.5 ? 1.0 : 0.0);
        }
        col = cbest; seen = sbest;
    }
    return float4(col, seen > 0.5 ? 1.0 : 0.0);
}
// The plate of eye r at a place of that eye's picture (pixels; it may be in the
// margin). .a its disparity; under -1000: nothing there.
float4 PlateAt(Texture2D t, int r, float2 q)
{
    const float2 P = g_size + 2.0 * g_pad.xy;
    const float2 pp = q + g_pad.xy;
    if (pp.x < 0.5 || pp.x > P.x - 0.5 || pp.y < 0.5 || pp.y > P.y - 0.5) return float4(0, 0, 0, -1e4);
    return t.SampleLevel(sampLinear, float2(((float)r * P.x + pp.x) / (2.0 * P.x), pp.y / P.y), 0);
}
// What neither eye sees is behind something: it is given the background beside
// it, carried on as in a mirror -- the pixel as far beyond the gap's FARTHER
// edge as this one is inside it. (A single colour dragged across the gap, as
// before, showed as streaks.) (tex0: PSCompose's, .a 1 seen. tex2: the new
// views' disparity. g_size: the full eye.)
float4 PSUnseen(VSOut i) : SV_Target
{
    const int W = (int)g_size.x;
    const int pane = (int)i.pos.x >= W ? 1 : 0;
    const int2 p = int2((int)i.pos.x - pane * W, (int)i.pos.y);
    const float4 v = tex0.Load(int3(pane * W + p.x, p.y, 0));
    if (v.a > 0.5) return float4(v.rgb, 1);
    // (Before all: a background painted for this picture -- tex4, the LEFT eye with every near
    // thing lifted off at its edges and what is behind it painted in, and that background's own
    // disparity in .a. It is looked up as the plate is: where the surface behind this gap would be
    // in the left eye, by the layer's own disparity there.)
    [branch] if (g_plate.w > 0.5)
    {
        const int w0 = (int)g_srcSize.x, h0 = (int)g_srcSize.y;
        const float2 full = float2((float)p.x + 0.5, (float)p.y + 0.5);
        const float2 uv = full / g_size;
        const float t = pane == 0 ? g_tL : g_tR;
        const float db = tex2.Load(int3(pane * w0 + clamp((int)(full.x / g_scale.x), 0, w0 - 1), clamp((int)(full.y / g_scale.y), 0, h0 - 1), 0)).r;
        // (Begun from the gap's far side as the geometry has it; and, failing that, from the painted
        // background's own disparity hereabouts -- the two are guesses of the same thing.)
        const float dHere = tex4.SampleLevel(sampLinear, clamp(uv, 0.001, 0.999), 0).a;
        float3 got = 0; bool hit = false;
        [unroll] for (int st = 0; st < 2; ++st)
        {
        float d = st == 0 ? db : min(db, dHere); float4 ly = float4(0, 0, 0, -1e4); bool ok = false, live = !hit;
        [unroll] for (int it = 0; it < 3; ++it)
        {
            const float2 q = full + d * Move(0.0, t, uv) * g_scale;
            const bool inside = q.x >= 0.5 && q.x <= g_size.x - 0.5 && q.y >= 0.5 && q.y <= g_size.y - 0.5;
            const float4 s = tex4.SampleLevel(sampLinear, clamp(q / g_size, 0.001, 0.999), 0);
            const bool near3 = inside && abs(s.a - d) <= 6.0;
            if (live) { ok = near3 && abs(s.a - d) < (it == 0 ? 6.0 : 1.0); if (near3) { ly = s; d = s.a; } }
            live = live && near3;
        }
        if (!hit && ok && d <= db + 2.0) { got = ly.rgb; hit = true; }
        }
        if (hit) return float4(g_plate.z > 0.5 ? float3(1, 1, 0) : got, 1);
    }
    // (First: was it in sight in an earlier picture? The plate has what was
    // farthest at each place of each eye. The surface behind this gap -- the
    // farther side's disparity -- is looked for in the plate where each eye
    // would have it; the plate's own disparity there is then believed and the
    // place looked up again, and it stands if the two agree.)
    [branch] if (g_plate.y > 0.5)
    {
        const int w0 = (int)g_srcSize.x, h0 = (int)g_srcSize.y;
        const float2 full = float2((float)p.x + 0.5, (float)p.y + 0.5);
        const float2 uv = full / g_size;
        const float t = pane == 0 ? g_tL : g_tR;
        const float db = tex2.Load(int3(pane * w0 + clamp((int)(full.x / g_scale.x), 0, w0 - 1), clamp((int)(full.y / g_scale.y), 0, h0 - 1), 0)).r;
        const int first = abs(t) <= abs(t - 1.0) ? 0 : 1;
        float3 got = 0; bool hit = false;
        [unroll] for (int e = 0; e < 2; ++e)
        {
            const int r = e == 0 ? first : 1 - first;
            const bool allowed = !(g_only > -0.5 && g_only < 1.5 && abs(g_only - (float)r) > 0.5);
            float d = db; float4 pl = float4(0, 0, 0, -1e4); bool ok = false, live = allowed;
            [unroll] for (int it = 0; it < 3; ++it)
            {
                const float2 q = full + d * Move((float)r, t, uv) * g_scale;
                const float4 s = PlateAt(tex1, r, q);
                const bool near3 = s.a > -1000.0 && abs(s.a - d) <= 3.0;
                if (live) { ok = near3 && abs(s.a - d) < (it == 0 ? 3.0 : 0.5); if (near3) { pl = s; d = s.a; } }
                live = live && near3;
            }
            if (!hit && ok && d <= db + 1.0) { got = pl.rgb; hit = true; }
        }
        if (hit) return float4(g_plate.z > 0.5 ? float3(1, 0, 1) : got, 1);
    }
    // (Then: what lies BEHIND, carried in. tex3 is the picture made smaller and
    // smaller, each texel keeping only the farthest of what it covers
    // (PSFillDown). The gap takes the finest of those in which something as
    // far as its own far side is found beside it -- near the gap's far edge a
    // fine one, deep inside a coarse one -- so the background runs on into the
    // gap, soft, where a mirrored row left streaks.)
    [branch] if (g_pad.z > 0.5)
    {
        const int w0 = (int)g_srcSize.x, h0 = (int)g_srcSize.y;
        const float2 full = float2((float)p.x + 0.5, (float)p.y + 0.5);
        const float db = tex2.Load(int3(pane * w0 + clamp((int)(full.x / g_scale.x), 0, w0 - 1), clamp((int)(full.y / g_scale.y), 0, h0 - 1), 0)).r;
        float3 acc = 0; float left = 1.0;
        const int levels = (int)g_pad.z;
        [loop] for (int m = 0; m < levels && left > 0.02; ++m)
        {
            uint mw, mh, mn; tex3.GetDimensions((uint)m, mw, mh, mn);
            const int ew = (int)mw / 2;
            const float2 u = full * float2((float)ew, (float)mh) / g_size - 0.5;
            const int2 b = int2((int)floor(u.x), (int)floor(u.y));
            const float2 f = u - float2(b);
            float3 c = 0; float sw = 0;
            [unroll] for (int k = 0; k < 4; ++k)
            {
                const int2 q = clamp(b + int2(k & 1, k >> 1), 0, int2(ew - 1, (int)mh - 1));
                const float4 t = tex3.Load(int3(pane * ew + q.x, q.y, m));
                const float wk = ((k & 1) ? f.x : 1.0 - f.x) * ((k >> 1) ? f.y : 1.0 - f.y);
                // (As far as the gap's far side, or farther; a little leave the coarser it is.)
                // (... by degrees, not at a line: a line shows as blocks.)
                const float far = saturate(db + 1.5 + 0.5 * (float)m - t.a);
                c += t.rgb * wk * far; sw += wk * far;
            }
            // (A level counts in full only where all four round the pixel have it; where fewer, the
            // coarser ones make up the rest -- two or three texels alone showed as blocks too.)
            const float take = left * saturate(sw * 1.6 - 0.6);
            if (sw > 1e-4) acc += c / sw * take;
            left -= take;
        }
        if (left < 0.5) return float4(g_plate.z > 0.5 ? float3(0, 0, 1) : acc / (1.0 - left), 1);
    }
    const int w = (int)g_srcSize.x, h = (int)g_srcSize.y;
    const int ly = clamp((int)((float)p.y / g_scale.y), 0, h - 1);
    int nl = 0, nr = 0; float dl = 1e30, dr = 1e30;
    // (However wide the gap: four pixels at a stride until something seen, then
    // back to its first pixel -- a quarter of the reads of walking it.)
    int sl = 0, sr = 0;
    [loop] for (int a = 4; a <= 4096 && p.x - a >= 0; a += 4)
        if (tex0.Load(int3(pane * W + p.x - a, p.y, 0)).a > 0.5) { sl = a; break; }
    if (sl == 0) sl = p.x + 1;
    [loop] for (int l = max(sl - 3, 1); l <= sl && p.x - l >= 0; ++l)
        if (tex0.Load(int3(pane * W + p.x - l, p.y, 0)).a > 0.5) { nl = l; dl = tex2.Load(int3(pane * w + clamp((int)((float)(p.x - l) / g_scale.x), 0, w - 1), ly, 0)).r; break; }
    [loop] for (int b = 4; b <= 4096 && p.x + b < W; b += 4)
        if (tex0.Load(int3(pane * W + p.x + b, p.y, 0)).a > 0.5) { sr = b; break; }
    if (sr == 0) sr = W - p.x;
    [loop] for (int r = max(sr - 3, 1); r <= sr && p.x + r < W; ++r)
        if (tex0.Load(int3(pane * W + p.x + r, p.y, 0)).a > 0.5) { nr = r; dr = tex2.Load(int3(pane * w + clamp((int)((float)(p.x + r) / g_scale.x), 0, w - 1), ly, 0)).r; break; }
    if (g_plate.z > 0.5) return float4(0, 1, 0, 1);   // (a test: what the plate did not have, marked)
    if (nl == 0 && nr == 0) return float4(v.rgb, 1);
    const bool left = nr == 0 || (nl != 0 && dl < dr);
    // (The mirrored pixel, if it was seen; else the edge's own.)
    const int edge = left ? p.x - nl : p.x + nr;
    const int mirror = clamp(left ? edge - (nl - 1) : edge + (nr - 1), 0, W - 1);
    const float4 m = tex0.Load(int3(pane * W + mirror, p.y, 0));
    return float4(m.a > 0.5 ? m.rgb : tex0.Load(int3(pane * W + edge, p.y, 0)).rgb, 1);
}

// ---- The disparity made finer ----------------------------------------------
// The matching above is on a small copy of the eyes, so its answer is good to a
// part of a small texel -- a pixel or so of the picture itself. That is depth in
// steps: far scenery, whose disparities differ by less than that, comes out
// flat. Here each texel's answer is tried again against the picture at its full
// size: a quarter and a half of a texel either side, the best kept, and between
// those by a parabola. Only a little either side, so it can sharpen an answer
// and cannot change which surface it is on.
// (tex0: the two real eyes, full size; g_srcSize one eye's size. tex1: the
// disparity, g_size.)
float3 FineAt(int half, float2 q)
{
    const float2 S = g_srcSize;
    return sqrt(max(tex0.SampleLevel(sampLinear, float2(((float)half * S.x + clamp(q.x, 0.5, S.x - 0.5)) / (2.0 * S.x), q.y / S.y), 0).rgb, 0.0));
}
float4 PSFine(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const int w = (int)g_size.x;
    const float4 own = tex1.Load(int3(half * w + p.x, p.y, 0));
    const float2 k = g_srcSize / g_size;                     // (picture pixels a texel)
    const float2 c = (float2(p) + 0.5) * k;
    const float sgn = half == 0 ? 1.0 : -1.0;
    float c5[5] = { 0, 0, 0, 0, 0 };
    const int R = (int)g_winR;
    [loop] for (int j = -R; j <= R; ++j)
    [loop] for (int n = -R; n <= R; ++n)
    {
        const float2 q = c + float2(n, j) * k / g_winR;
        const float3 a = FineAt(half, q);
        [unroll] for (int m = 0; m < 5; ++m)
        {
            const float xb = q.x - sgn * (own.r + (float)(m - 2) * 0.25) * k.x;
            if (xb < 0.0 || xb > g_srcSize.x) { c5[m] += kOut; continue; }
            c5[m] += min(dot(abs(a - FineAt(1 - half, float2(xb, q.y))), 1.0 / 3.0), kCapA);
        }
    }
    float best = c5[0]; int bm = 0;
    [unroll] for (int m2 = 1; m2 < 5; ++m2) if (c5[m2] < best) { best = c5[m2]; bm = m2; }
    // (Its own stands unless another is clearly better; at the end of the five
    // there is no telling where the best lies, so its own stands there too.)
    if (bm == 0 || bm == 4 || (bm != 2 && best > c5[2] * 0.95)) bm = 2;
    float cm = 0, cp = 0;
    [unroll] for (int m3 = 0; m3 < 5; ++m3) { cm = (m3 == bm - 1) ? c5[m3] : cm; cp = (m3 == bm + 1) ? c5[m3] : cp; }
    const float den = cm - 2.0 * c5[bm] + cp;
    const float flat = (2.0 * g_winR + 1.0) * (2.0 * g_winR + 1.0) * 0.5 / 255.0;
    const float sub = den > flat ? clamp(0.5 * (cm - cp) / den, -0.5, 0.5) : 0.0;
    if (bm == 2 && den <= flat) return own;    // (nothing there to match on)
    return float4(own.r + ((float)(bm - 2) + sub) * 0.25, own.g, own.b, 0);
}

// ---- The plate: what was in sight before -------------------------------------
// Two eyes see only what two eyes see: what is behind a thing from both cannot
// be had from this picture at all. But a moving picture showed it a moment ago
// -- the thing was elsewhere, or the camera was. So each eye keeps a plate: at
// every place, the FARTHEST surface seen there lately, its colour and its
// disparity. Each new picture the old plate is first moved as the picture
// moved (g_motion: a shift for everything, and more for the nearer -- a camera
// turning and a camera stepping sideways); then a place takes the new
// picture's surface where that is as far as the plate's or farther, and keeps
// the plate's where the new one is nearer -- something has come in front.
// (tex0: the eyes, full size, g_size. tex1: their disparity, g_srcSize.
// tex2: the plate before, full size; .a its disparity, under -1000 none.)
float4 PSPlate(VSOut i) : SV_Target
{
    // (The plate is the eye's picture and a margin round it.)
    const float2 P = g_size + 2.0 * g_pad.xy;
    const int half = i.pos.x >= P.x ? 1 : 0;
    const float2 pf = float2(i.pos.x - (float)half * P.x, i.pos.y) - g_pad.xy;
    const bool inPic = pf.x > 0.0 && pf.x < g_size.x && pf.y > 0.0 && pf.y < g_size.y;
    const int2 p = clamp(int2((int)pf.x, (int)pf.y), 0, int2((int)g_size.x - 1, (int)g_size.y - 1));
    const int W = (int)g_size.x, w = (int)g_srcSize.x, h = (int)g_srcSize.y;
    const float3 cur = tex0.Load(int3(half * W + p.x, p.y, 0)).rgb;
    const float2 lo = clamp(pf / g_scale, 0.5, float2(w, h) - 0.5);
    const float dcur = tex1.SampleLevel(sampLinear, float2(((float)(half * w) + lo.x) / (float)(2 * w), lo.y / (float)h), 0).r;
    // (A change of scene: little of the new picture is the old one moved -- the
    // motion finder's count (tex4 .g) is low. The plate is begun again.)
    const bool cut = g_lk.w > 0.5 && tex4.Load(int3(0, 0, 0)).g < 0.6;
    if (g_plate.x < 0.5 || cut) return inPic ? float4(cur, dcur) : float4(0, 0, 0, -1e4);
    // (Where the plate's surface here was before: by its own disparity, which is
    // not known until it is found -- the new picture's first, then its own.)
    // (Begun from the far side: begun from a near thing's own disparity the
    // search lands where that thing was, and finds the thing. So the farthest
    // the new picture has within reach along the row is where it begins.)
    float dfar = dcur;
    static const int kR[8] = { -2, 2, -6, 6, -14, 14, -30, 30 };
    [unroll] for (int n = 0; n < 8; ++n)
    {
        const float xn = clamp(lo.x + (float)kR[n], 0.5, (float)w - 0.5);
        dfar = min(dfar, tex1.SampleLevel(sampLinear, float2(((float)(half * w) + xn) / (float)(2 * w), lo.y / (float)h), 0).r);
    }
    const float4 mo = g_motion + (g_lk.w > 0.5 ? tex3.Load(int3(0, 0, 0)) : float4(0, 0, 0, 0));
    float d = dfar; float4 pl = float4(0, 0, 0, -1e4); bool have = true;
    [unroll] for (int it = 0; it < 3; ++it)
    {
        const float2 q = pf - (mo.xy + d * g_scale * mo.zw);
        const float4 s = PlateAt(tex2, half, q);
        have = have && s.a > -1000.0;
        if (have) { pl = s; d = s.a; }
    }
    // (In the margin there is no new picture: the plate's, moved, or nothing.)
    if (!inPic) return have ? pl : float4(0, 0, 0, -1e4);
    if (!have || dcur <= pl.a + 0.75) return float4(cur, dcur);
    return pl;
}

// ---- How the picture moved ----------------------------------------------------
// For the plate to be carried from one picture to the next it must be known how
// the picture moved. A camera that turns moves everything alike; one that steps
// sideways or up moves each thing by its nearness -- by its disparity, which is
// known. So four numbers are looked for: a shift (x, y) and a shift for each
// unit of disparity (x, y). They are found the usual way for a small motion:
// where the picture's brightness slopes, the change in brightness between the
// two pictures says how far it moved along the slope; all the picture's texels
// together say it for the four numbers (least squares), a texel that does not
// fit -- something moving of itself -- counting for little. From the smallest
// level to the finest, each starting from the one before.
// PSLKSum: 16 x 16 tiles of sums, into four targets. (tex0: this level's
// colour, now. tex1: the same of the picture before. tex2: this level's
// disparity. tex3: the motion so far, one texel. The left eye only.)
struct LKOut { float4 a0 : SV_Target0; float4 a1 : SV_Target1; float4 a2 : SV_Target2; float4 a3 : SV_Target3; };
LKOut PSLKSum(VSOut i)
{
    const int T = 16;
    const int2 tile = int2((int)i.pos.x, (int)i.pos.y);
    const int w = (int)g_size.x, h = (int)g_size.y;
    const int bw = (w + T - 1) / T, bh = (h + T - 1) / T;
    const float4 g = tex3.Load(int3(0, 0, 0));
    float4 s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    [loop] for (int y = 0; y < bh; ++y)
    [loop] for (int x = 0; x < bw; ++x)
    {
        const int2 p = tile * int2(bw, bh) + int2(x, y);
        if (p.x < 2 || p.x >= w - 2 || p.y < 2 || p.y >= h - 2) continue;
        const float dfull = tex2.Load(int3(p, 0)).r * g_lk.z;
        const float2 flow = (g.xy + dfull * g.zw) / g_lk.x;
        const float2 q = float2(p) + 0.5 - flow;
        if (q.x < 1.0 || q.x > (float)w - 1.0 || q.y < 1.0 || q.y > (float)h - 1.0) continue;
        const float I1 = Luma(tex0.Load(int3(p, 0)).rgb);
        const float I0 = Luma(tex1.SampleLevel(sampLinear, float2(q.x / (float)(2 * w), q.y / (float)h), 0).rgb);
        const float Gx = 0.5 * (Luma(tex0.Load(int3(p + int2(1, 0), 0)).rgb) - Luma(tex0.Load(int3(p - int2(1, 0), 0)).rgb));
        const float Gy = 0.5 * (Luma(tex0.Load(int3(p + int2(0, 1), 0)).rgb) - Luma(tex0.Load(int3(p - int2(0, 1), 0)).rgb));
        const float r = I1 - I0;
        const float wt = 1.0 / (1.0 + (r / 0.06) * (r / 0.06));
        const float dn = dfull / g_lk.y;
        const float4 J = float4(Gx, Gy, dn * Gx, dn * Gy);
        s0 += wt * J.x * J;                                    // (the matrix, row by row: 00 01 02 03)
        s1 += wt * float4(J.y * J.y, J.y * J.z, J.y * J.w, J.z * J.z);   // (11 12 13 22)
        s2 += wt * float4(J.z * J.w, J.w * J.w, -J.x * r, -J.y * r);     // (23 33, and the right side 0 1)
        s3 += wt * float4(-J.z * r, -J.w * r, abs(r), 1.0);              // (the right side 2 3; how badly it fits; how many)
    }
    LKOut o; o.a0 = s0; o.a1 = s1; o.a2 = s2; o.a3 = s3;
    return o;
}
// PSLKSolve: the tiles added up and the four numbers put right by what they
// say. (tex0..3: the sums. tex4: the motion so far.) The second target: how
// badly the picture fits the motion (.r -- high at a change of scene), and the
// share of the picture that counted (.g).
struct SolveOut { float4 g : SV_Target0; float4 q : SV_Target1; };
SolveOut PSLKSolve(VSOut i)
{
    float4 s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    [loop] for (int y = 0; y < 16; ++y)
    [loop] for (int x = 0; x < 16; ++x)
    {
        s0 += tex0.Load(int3(x, y, 0)); s1 += tex1.Load(int3(x, y, 0));
        s2 += tex2.Load(int3(x, y, 0)); s3 += tex3.Load(int3(x, y, 0));
    }
    float A[4][4] = { { s0.x, s0.y, s0.z, s0.w }, { s0.y, s1.x, s1.y, s1.z }, { s0.z, s1.y, s1.w, s2.x }, { s0.w, s1.z, s2.x, s2.y } };
    float b[4] = { s2.z, s2.w, s3.x, s3.y };
    // (What the picture cannot say -- no slope, or everything at one depth -- is left as it was.)
    const float lam = 1e-3 * (A[0][0] + A[1][1] + A[2][2] + A[3][3]) * 0.25 + 1e-12;
    [unroll] for (int k = 0; k < 4; ++k) A[k][k] += lam;
    [unroll] for (int c = 0; c < 4; ++c)
    {
        const float inv = 1.0 / A[c][c];
        [unroll] for (int r = c + 1; r < 4; ++r)
        {
            const float f = A[r][c] * inv;
            [unroll] for (int m = c; m < 4; ++m) A[r][m] -= f * A[c][m];
            b[r] -= f * b[c];
        }
    }
    float d[4] = { 0, 0, 0, 0 };
    [unroll] for (int c2 = 3; c2 >= 0; --c2)
    {
        float v = b[c2];
        [unroll] for (int m2 = c2 + 1; m2 < 4; ++m2) v -= A[c2][m2] * d[m2];
        d[c2] = v / A[c2][c2];
    }
    // (In this level's texels; no more than a texel and a half at a step.)
    float4 dl = clamp(float4(d[0], d[1], d[2], d[3]), -1.5, 1.5);
    SolveOut o;
    o.g = tex4.Load(int3(0, 0, 0)) + float4(dl.xy * g_lk.x, dl.zw * g_lk.x / g_lk.y);
    o.q = float4(s3.z / max(s3.w, 1e-6), s3.w / max(g_size.x * g_size.y, 1.0), 0, 0);
    return o;
}

// ---- A disparity from another matcher, put right in its detail -----------------
// A trained matcher knows which surface a texel is on far better than the
// windows above -- outlines, thin things, plain areas -- but it is good to half
// a pixel or so, where two eyes mixed into one view want a tenth. So its
// answer is kept as to WHERE and put right as to HOW MUCH: the candidates
// are its own disparity and two texels either side, no further -- it cannot
// be moved onto another surface -- costed as PSMatchRefine costs its five, and
// between them by the parabola.
// (tex0: the level. tex1: the other matcher's disparity, this size. g_winR.)
float4 PSGuide(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const int w = (int)g_size.x, h = (int)g_size.y;
    const int R = (int)g_winR;
    const int sgn = half == 0 ? 1 : -1;
    const float4 own = tex1.Load(int3(half * w + p.x, p.y, 0));
    const int c0 = (int)round(own.r);
    float c5[5] = { 0, 0, 0, 0, 0 };
    [loop] for (int j = -R; j <= R; ++j)
    [loop] for (int k = -R; k <= R; ++k)
    {
        const int xa = clamp(p.x + k, 0, w - 1), ya = clamp(p.y + j, 0, h - 1);
        // (Only the window's texels on the same surface count: the other matcher says which they are.)
        const float dn = tex1.Load(int3(half * w + xa, ya, 0)).r;
        if (abs(dn - own.r) > 1.5) continue;
        const float4 a = tex0.Load(int3(half * w + xa, ya, 0));
        [unroll] for (int m = 0; m < 5; ++m)
        {
            const int xb = xa - sgn * (c0 + m - 2);
            if (xb < 0 || xb >= w) { c5[m] += kOut; continue; }
            const float4 b = tex0.Load(int3((1 - half) * w + xb, ya, 0));
            c5[m] += 0.35 * min(dot(abs(a.rgb - b.rgb), 1.0 / 3.0), kCapA) + 0.65 * min(abs(a.a - b.a), kCapG);
        }
    }
    // (Its own stands unless another is clearly better: a plain area says nothing.)
    int bm = 2; float best = c5[2] * 0.92;
    [unroll] for (int m2 = 0; m2 < 5; ++m2) if (m2 != 2 && c5[m2] < best) { best = c5[m2]; bm = m2; }
    if (bm == 2) best = c5[2];
    float sub = 0.0;
    if (bm > 0 && bm < 4)
    {
        float cm = 0, cp = 0;
        [unroll] for (int m3 = 0; m3 < 5; ++m3) { cm = (m3 == bm - 1) ? c5[m3] : cm; cp = (m3 == bm + 1) ? c5[m3] : cp; }
        const float den = cm - 2.0 * best + cp;
        sub = den > 1e-5 ? clamp(0.5 * (cm - cp) / den, -0.5, 0.5) : 0.0;
        // (Where its own stood and the costs say nothing, its own fraction is kept.)
        if (bm == 2 && den <= 2.0 / 255.0) return own;
    }
    return float4(clamp((float)(c0 + bm - 2) + sub, own.r - 2.0, own.r + 2.0), own.g, own.b, 0);
}

// ---- Two matchers' answers, the better taken at each texel ---------------------
// The trained matcher is right about outlines and wrong, sometimes, about a
// whole soft or plain region; the windows above are the other way about. Each
// answer is costed here on its own terms -- over the window's texels that ITS
// map puts on the texel's own surface, so a thing's outline does not count
// against the ground behind it -- and at its own fraction of a texel. The
// trained one stands unless the other is clearly the better fit.
// (tex0: the level. tex1: the trained matcher's disparity. tex2: the windows'.)
float FuseCost(Texture2D map, int2 p, int half, int w, int h)
{
    const float d0 = map.Load(int3(half * w + p.x, p.y, 0)).r;
    const float sgn = half == 0 ? 1.0 : -1.0;
    float s = 0, n = 0;
    [loop] for (int j = -2; j <= 2; ++j)
    [loop] for (int k = -2; k <= 2; ++k)
    {
        const int xa = clamp(p.x + k, 0, w - 1), ya = clamp(p.y + j, 0, h - 1);
        if (abs(map.Load(int3(half * w + xa, ya, 0)).r - d0) > 1.5) continue;
        const float xb = (float)xa + 0.5 - sgn * d0;
        if (xb < 0.5 || xb > (float)w - 0.5) continue;
        const float4 a = tex0.Load(int3(half * w + xa, ya, 0));
        const float4 b = tex0.SampleLevel(sampLinear, float2(((float)((1 - half) * w) + xb) / (float)(2 * w), ((float)ya + 0.5) / (float)h), 0);
        s += 0.35 * min(dot(abs(a.rgb - b.rgb), 1.0 / 3.0), kCapA) + 0.65 * min(abs(a.a - b.a), kCapG);
        n += 1.0;
    }
    // (Few texels to go on: little is known, and that counts against it a little.)
    return n > 0.5 ? s / n + 0.004 * (25.0 - n) / 25.0 : 1.0;
}
float4 PSFuse(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const int w = (int)g_size.x, h = (int)g_size.y;
    const float4 nn = tex1.Load(int3(half * w + p.x, p.y, 0));
    const float4 own = tex2.Load(int3(half * w + p.x, p.y, 0));
    // (At an outline the trained matcher is the one to believe: within two texels
    // of a step in ITS map, its answer stands. The windows are fat there.)
    float lo = nn.r, hi = nn.r;
    static const int2 kE[8] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1), int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2) };
    [unroll] for (int e = 0; e < 8; ++e)
    {
        const float v = tex1.Load(int3(InEye(p + kE[e], half, w, h), 0)).r;
        lo = min(lo, v); hi = max(hi, v);
    }
    if (g_opt.z > 0.5 && hi - lo > 1.5) return float4(nn.r, nn.g, nn.b, 0);
    if (abs(nn.r - own.r) < 0.25) return float4(own.r, nn.g, nn.b, 0);     // (they agree: the windows' fraction is the finer)
    const float cn = FuseCost(tex1, p, half, w, h), co = FuseCost(tex2, p, half, w, h);
    // (g_opt.x: how far from the trained matcher's answer the windows' may be and still be taken. It
    // says which surface; the windows only say more finely where on it. Without that limit a see-
    // through or plain thing came out in patches of two depths -- seen twice, once at each.)
    // (... where it is sure, that is: where its answer for this eye and its answer for the other eye,
    // made apart, meet at the same point. Where they do not -- a soft or plain region it has guessed
    // at, or what one eye cannot see -- the choice is open as before.)
    const float xo = (float)p.x + 0.5 - (half == 0 ? 1.0 : -1.0) * nn.r;
    const bool inO = xo > 0.5 && xo < (float)w - 0.5;
    const float nnO = tex1.SampleLevel(sampLinear, float2(((float)((1 - half) * w) + clamp(xo, 0.5, (float)w - 0.5)) / (float)(2 * w), ((float)p.y + 0.5) / (float)h), 0).r;
    const bool sure = inO && abs(nnO - nn.r) < 1.0;
    if (g_opt.x > 0.0 && sure && abs(own.r - nn.r) > g_opt.x) return float4(nn.r, nn.g, nn.b, 0);
    return (co < cn * g_opt.w - 0.5 / 255.0) ? float4(own.r, own.g, own.b, 0) : float4(nn.r, nn.g, nn.b, 0);
}

// ---- The far side of things, for filling what is hidden ----------------------
// The new view made smaller, level by level; each texel keeps the FARTHEST of
// the four it covers -- their disparity, and the colour of those of the four
// that are about that far. What neither eye saw is left out. Near things are
// thus dropped level by level and what is behind them spreads: at some level
// every gap has background beside it.
// g_eye 0, the first level: tex0 the new view at full size (.a 1 seen), tex2
// its disparity (g_srcSize, g_scale). g_eye 1: tex0 the level before.
// g_size: this level's eye. Out: colour, and in .a the disparity (10000: nothing).
float4 PSFillDown(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    uint sw, sh; tex0.GetDimensions(sw, sh);
    const int ew = (int)sw / 2;
    float4 ch[4]; float dmin = 1e4;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2 q = clamp(p * 2 + int2(k & 1, k >> 1), 0, int2(ew - 1, (int)sh - 1));
        float4 t = tex0.Load(int3(half * ew + q.x, q.y, 0));
        if (g_eye < 0.5)
        {
            const int w0 = (int)g_srcSize.x, h0 = (int)g_srcSize.y;
            const float d = tex2.Load(int3(half * w0 + clamp((int)(((float)q.x + 0.5) / g_scale.x), 0, w0 - 1), clamp((int)(((float)q.y + 0.5) / g_scale.y), 0, h0 - 1), 0)).r;
            // (Not one beside a nearer thing: at an outline the colours of the two sides run together.)
            const int2 mq = int2(clamp((int)(((float)q.x + 0.5) / g_scale.x), 0, w0 - 1), clamp((int)(((float)q.y + 0.5) / g_scale.y), 0, h0 - 1));
            float nearest = d;
            [unroll] for (int e = 0; e < 8; ++e)
            {
                const int2 o = int2((e & 3) == 0 ? -1 : (e & 3) == 1 ? 1 : 0, (e & 3) == 2 ? -1 : (e & 3) == 3 ? 1 : 0) * (e < 4 ? 1 : 2);
                nearest = max(nearest, tex2.Load(int3(half * w0 + clamp(mq.x + o.x, 0, w0 - 1), clamp(mq.y + o.y, 0, h0 - 1), 0)).r);
            }
            t.a = (t.a > 0.5 && nearest < d + 1.0) ? d : 1e4;
        }
        ch[k] = t; dmin = min(dmin, t.a);
    }
    if (dmin > 5000.0) return float4(0, 0, 0, 1e4);
    float3 c = 0; float n = 0;
    [unroll] for (int k2 = 0; k2 < 4; ++k2) if (ch[k2].a < dmin + 0.75) { c += ch[k2].rgb; n += 1.0; }
    return float4(c / n, dmin);
}

// ---- A thing's outline taken a texel wide ---------------------------------------
// A sharp map cuts a thing out at its outline exactly -- and the outline's own
// pixels are part thing, part what is behind: fur, hair, a soft edge. Left on
// the far side they stay behind when the thing moves, a rim of it at the wrong
// depth, seen twice. So at a step the nearer side is carried one texel
// outward, and the rim goes with the thing it belongs to.
// (tex1: the disparity. g_winR: the reach, in texels.)
float4 PSDilate(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const int w = (int)g_size.x, h = (int)g_size.y;
    const float4 own = tex1.Load(int3(half * w + p.x, p.y, 0));
    const int R = (int)g_winR;
    float best = own.r;
    [loop] for (int j = -R; j <= R; ++j)
    [loop] for (int k = -R; k <= R; ++k)
    {
        const float v = tex1.Load(int3(InEye(p + int2(k, j), half, w, h), 0)).r;
        if (v > own.r + 1.5) best = max(best, v);
    }
    return float4(best, own.g, own.b, 0);
}

// ---- The picture made small, for a matcher elsewhere ---------------------------
// Both eyes at a size a trained matcher wants (src/NeuralDepth): each pixel the
// mean of sixteen samples over what it covers. (tex0: the eyes, g_srcSize one
// eye's size. g_size: the small eye.)
float4 PSShrink(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const float2 S = g_srcSize, k = S / g_size;
    float3 s = 0;
    [unroll] for (int y = 0; y < 4; ++y)
    [unroll] for (int x = 0; x < 4; ++x)
    {
        const float2 q = (float2(p) + (float2(x, y) + 0.5) / 4.0) * k;
        s += tex0.SampleLevel(sampLinear, float2(((float)half * S.x + clamp(q.x, 0.5, S.x - 0.5)) / (2.0 * S.x), q.y / S.y), 0).rgb;
    }
    return float4(s / 16.0, 1);
}

// ---- The right eye's disparity from the left's ----------------------------------
// A trained matcher is asked for the left eye only -- half the work. The right
// eye's map is the same surfaces seen from a little to the side: a right-eye
// texel has the disparity of the left-eye texel that lands on it (x - d), and
// where more than one lands, the nearest's. Looked for from the nearest
// disparity down, so the first that fits is it. Where none lands -- behind
// something as the left eye has it -- PSRightFill gives the farther side's.
// (tex1: both eyes' disparity, the left half filled. g_range.)
float4 PSRight(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const int w = (int)g_size.x;
    if (half == 0) return tex1.Load(int3(p.x, p.y, 0));
    const int R = min((int)g_range, 96);
    float best = -1e4;
    [loop] for (int k = R; k >= -R; --k)
    {
        const int xl = p.x + k;
        if (xl < 0 || xl >= w) continue;
        const float d = tex1.Load(int3(xl, p.y, 0)).r;
        if (abs(d - (float)k) < 0.75) { best = d; break; }
    }
    return float4(best, 1, 0, 0);
}
float4 PSRightFill(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const int w = (int)g_size.x;
    const float4 own = tex1.Load(int3(half * w + p.x, p.y, 0));
    if (half == 0 || own.r > -1000.0) return own;
    float d = 0.0; bool have = false;
    [loop] for (int l = 1; l <= 96 && p.x - l >= 0; ++l)
    {
        const float q = tex1.Load(int3(w + p.x - l, p.y, 0)).r;
        if (q > -1000.0) { d = q; have = true; break; }
    }
    [loop] for (int r = 1; r <= 96 && p.x + r < w; ++r)
    {
        const float q = tex1.Load(int3(w + p.x + r, p.y, 0)).r;
        if (q > -1000.0) { d = have ? min(d, q) : q; have = true; break; }
    }
    return float4(d, 1, 0, 0);
}

// ---- A step in depth kept a step -------------------------------------------------
// A trained matcher's map is small, and made larger its steps become ramps: a
// row or two of texels neither on the near thing nor on what is behind it, but
// in the space between. Thrown forward those show as lines hanging between the
// two. So where there is a real step within two texels, a texel that is on
// neither side of it is put on the nearer one of the two.
// (tex1: the disparity. Twice over, for a ramp wider than the window.)
float4 PSSnap(VSOut i) : SV_Target
{
    int half; int2 p; Place(i.pos, half, p);
    const int w = (int)g_size.x, h = (int)g_size.y;
    const float4 own = tex1.Load(int3(half * w + p.x, p.y, 0));
    float lo = own.r, hi = own.r;
    [unroll] for (int j = -2; j <= 2; ++j)
    [unroll] for (int k = -2; k <= 2; ++k)
    {
        const float v = tex1.Load(int3(InEye(p + int2(k, j), half, w, h), 0)).r;
        lo = min(lo, v); hi = max(hi, v);
    }
    const float span = hi - lo;
    if (span < 2.0) return own;
    const float t = (own.r - lo) / span;
    if (t < 0.25 || t > 0.75) return own;                  // (on one side already, near enough)
    return float4(lo, own.g, own.b, 0);                    // (to the far side: a texel in doubt is background)
}

// ---- Gaussian splats --------------------------------------------------------
// The new views made the other way round: not each pixel of the new view asking
// the eyes what it is, but each pixel of each eye thrown to where it is seen
// from the new place, as a Gaussian -- a soft round spot of its colour, its
// middle where the pixel's middle goes, as wide as the pixel and stretched
// along the row where the surface it lies on is stretched by the move. Where
// spots overlap they are mixed by their weights.
//
// Two layers are added up apart: the spots on the nearest surface at the place
// they land (tex2, the new views' disparity), and those behind it. A pixel on
// an outline is some of the thing and some of what is behind it: it is thrown
// twice, as a spot at the thing's depth with its share of the thing, and one at
// the far side's depth with the rest. The near layer is then laid over the far
// by its share -- so an outline is soft where the picture has it soft.
// (g_size: one eye, full size. g_srcSize: the small maps'. tex0: the two eyes.
// tex1: their disparity, small. g_lk.x: pixels a spot, each way. g_lk.y: a
// spot's width (sigma) in its own spacings. g_lk.z: the cover under which a
// pixel counts as reached by nothing. g_lk.w: 1 the outlines asked of the eyes.
// One point a spot, both eyes' one after the other, made a quad or two by
// GSGauss; drawn once for each new view, into its half.)
struct GaussPt
{
    float4 cm : TEXCOORD0;      // the spot's middle in its eye (pixels); how it moves a disparity (Move)
    float4 col : TEXCOORD1;     // its colour; how much its eye counts
    float4 dd : TEXCOORD2;      // the nearer disparity, the farther, the nearer's share, the row's stretch
    float keep : TEXCOORD3;
};
struct GaussOut { float4 pos : SV_POSITION; float2 off : TEXCOORD0; nointerpolation float4 col : TEXCOORD1; nointerpolation float2 da : TEXCOORD2; };
GaussPt GaussSpot(int ref, float2 c)
{
    const float step = max(g_lk.x, 1.0);
    const int w = (int)g_srcSize.x, h = (int)g_srcSize.y;
    const float t = g_eye < 0.5 ? g_tL : g_tR;
    // Its disparity: between the four texels round it where they are one surface; else the texel it is in.
    const float2 lo = c / g_scale - 0.5;
    const int x0 = clamp((int)floor(lo.x), 0, w - 2), y0 = clamp((int)floor(lo.y), 0, h - 2);
    const float2 f = saturate(lo - float2(x0, y0));
    const float d00 = tex1.Load(int3(ref * w + x0, y0, 0)).r, d10 = tex1.Load(int3(ref * w + x0 + 1, y0, 0)).r;
    const float d01 = tex1.Load(int3(ref * w + x0, y0 + 1, 0)).r, d11 = tex1.Load(int3(ref * w + x0 + 1, y0 + 1, 0)).r;
    const float dHi = max(max(d00, d10), max(d01, d11)), dLo = min(min(d00, d10), min(d01, d11));
    const bool one = dHi - dLo < 1.0;
    const float dNear = tex1.Load(int3(ref * w + clamp((int)floor(c.x / g_scale.x), 0, w - 1), clamp((int)floor(c.y / g_scale.y), 0, h - 1), 0)).r;
    float d1 = one ? lerp(lerp(d00, d10, f.x), lerp(d01, d11, f.x), f.y) : dNear, d2 = d1, share = 1.0;
    // (The eye further from the new place: only what the nearer eye does not see is drawn of it. Asked first,
    // before the outline's work: where the nearer eye's own map has both the surfaces this spot could be on,
    // there is nothing to do here.)
    const float awayE = abs(t - (float)ref), awayOE = abs(t - (float)(1 - ref));
    [branch] if (g_only < -0.5 && awayE > awayOE)
    {
        bool seenAll = true;
        [unroll] for (int e = 0; e < 2; ++e)
        {
            const float dq = one ? d1 : (e == 0 ? dHi : dLo);
            const float2 lm = c / g_scale + float2(ref == 1 ? dq : -dq, 0.0);
            const bool inside = lm.x >= 0.0 && lm.x < (float)w && lm.y >= 0.0 && lm.y < (float)h;
            const float dn = tex1.Load(int3((1 - ref) * w + clamp((int)floor(lm.x), 0, w - 1), clamp((int)floor(lm.y), 0, h - 1), 0)).r;
            seenAll = seenAll && inside && abs(dn - dq) < 1.0;
        }
        if (seenAll) { GaussPt z; z.cm = 0; z.col = 0; z.dd = 0; z.keep = 0.0; return z; }
    }
    // (At an outline -- the four are not one surface -- the map is a texel coarse, and its texels are four
    // pixels of the picture: which side of the outline THIS pixel is on, and how much of it, is asked of the
    // two eyes themselves. The pixel and the two beside it are set against what the other eye has where the
    // near side's disparity puts them, and where the far side's does. It is the near side's by as much as
    // that fits the better. Where neither fits, the other eye does not see this pixel at all -- it is behind
    // something -- and it is the far side's.)
    [branch] if (!one && g_lk.w > 0.5)
    {
        const float cand[2] = { dHi, dLo };
        float3 own[3];
        [unroll] for (int a = 0; a < 3; ++a)
            own[a] = sqrt(max(tex0.SampleLevel(sampLinear, float2(((float)ref * g_size.x + clamp(c.x + (float)(a - 1) * step, 0.5, g_size.x - 0.5)) / (2.0 * g_size.x), c.y / g_size.y), 0).rgb, 0.0));
        float cost[2] = { 0.0, 0.0 };
        [unroll] for (int k = 0; k < 2; ++k)
        {
            const float sh = (ref == 1 ? cand[k] : -cand[k]) * g_scale.x;
            [unroll] for (int b = 0; b < 3; ++b)
            {
                const float xo = c.x + (float)(b - 1) * step + sh;
                const float3 o2 = sqrt(max(tex0.SampleLevel(sampLinear, float2(((float)(1 - ref) * g_size.x + clamp(xo, 0.5, g_size.x - 0.5)) / (2.0 * g_size.x), c.y / g_size.y), 0).rgb, 0.0));
                cost[k] += dot(abs(own[b] - o2), 1.0 / 9.0);
                if (xo < 0.0 || xo > g_size.x) cost[k] += 1.0;
            }
        }
        d1 = dHi; d2 = dLo;
        share = min(cost[0], cost[1]) > 0.06 ? 0.0 : smoothstep(0.3, 0.7, cost[1] / (cost[0] + cost[1] + 1e-4));
        // (g_lk.w under 1.5: one side or the other, whole.)
        if (g_lk.w < 1.5) share = share > 0.5 ? 1.0 : 0.0;
    }
    const float2 mv = Move((float)ref, t, c / g_size);
    // (How the move stretches the surface along the row: a wall turned towards the new place is drawn out.)
    const float slope = one ? lerp(d10 - d00, d11 - d01, f.y) : 0.0;
    const float sx = clamp(abs(1.0 - slope * mv.x), 1.0, 3.0);
    // (How much this eye counts at the new place, against the eye that counts most there: far outside the
    // two eyes both count for next to nothing, and the nearer must still count in full.)
    const float away = abs(t - (float)ref), awayO = abs(t - (float)(1 - ref));
    const float wOwn = max(1.0 - away, 0.02 * exp(-2.0 * away)), wOther = max(1.0 - awayO, 0.02 * exp(-2.0 * awayO));
    const float wg = wOwn / max(wOwn, wOther);
    GaussPt o;
    o.cm = float4(c, mv);
    o.col = float4(tex0.SampleLevel(sampLinear, float2(((float)ref * g_size.x + c.x) / (2.0 * g_size.x), c.y / g_size.y), 0).rgb, wg);
    o.dd = float4(d1, d2, share, sx);
    o.keep = 1.0;
    if (g_only > -0.5 && g_only < 1.5 && abs(g_only - (float)ref) > 0.5) o.keep = 0.0;
    // (The eye further from the new place: only what the nearer eye
    // does not see is drawn of it -- where the nearer eye's own map has this surface, its own spot stands
    // for it. Half the spots, and nothing mixed that need not be.)
    [branch] if (g_only < -0.5 && away > awayO)
    {
        const float d = share > 0.5 ? d1 : d2;
        const float2 lm = c / g_scale + float2(ref == 1 ? d : -d, 0.0);
        const bool inside = lm.x >= 0.0 && lm.x < (float)w && lm.y >= 0.0 && lm.y < (float)h;
        const float dn = tex1.Load(int3((1 - ref) * w + clamp((int)floor(lm.x), 0, w - 1), clamp((int)floor(lm.y), 0, h - 1), 0)).r;
        if (inside && abs(dn - d) < 1.0) o.keep = 0.0;
    }
    return o;
}
GaussPt SpotOfId(uint id)
{
    const float step = max(g_lk.x, 1.0);
    const int gw = (int)ceil(g_size.x / step), gh = (int)ceil(g_size.y / step);
    const uint per = (uint)(gw * gh);
    // (Not in the picture's own order: runs of 128 spots, the runs taken from all over the picture one after
    // another. The card shares its drawing out by where on the screen it falls, and spots all in one place at
    // a time keep one part of it busy and the rest idle -- g_lk.w under 2.5: as they come.)
    const uint blocks = (per + 127u) / 128u;
    const uint slot = id % (blocks * 128u);
    const uint idx = g_pad.w > 0.5 ? ((slot >> 7) * 65521u) % blocks * 128u + (slot & 127u) : slot;
    GaussPt o = GaussSpot((int)(id / (blocks * 128u)), (float2((float)(idx % (uint)gw), (float)(min(idx, per - 1u) / (uint)gw)) + 0.5) * step);
    if (idx >= per) o.keep = 0.0;
    return o;
}
GaussPt VSGauss(uint id : SV_VertexID) { return SpotOfId(id); }
// The outlines only (see below): sixteen spots to each cell of the small map that CSCells listed.
StructuredBuffer<uint> g_cells : register(t5);
GaussPt VSGaussCell(uint k : SV_VertexID, uint inst : SV_InstanceID)
{
    const int w = (int)g_srcSize.x;
    const uint cellId = g_cells[inst];
    const int X = (int)(cellId % (uint)(2 * w)), y0 = (int)(cellId / (uint)(2 * w));
    const int ref = X >= w ? 1 : 0;
    const float2 c = (float2((float)(X - ref * w), (float)y0) + 0.5 + (float2((float)(k & 3u), (float)(k >> 2)) + 0.5) * 0.25) * g_scale;
    GaussPt o = GaussSpot(ref, min(c, g_size - 0.5));
    if (c.x >= g_size.x || c.y >= g_size.y) o.keep = 0.0;
    return o;
}
void GaussQuad(GaussPt i, float d, float share, inout TriangleStream<GaussOut> s)
{
    const float step = max(g_lk.x, 1.0);
    const float2 p = i.cm.xy - d * i.cm.zw * g_scale;
    const float2 sig = g_lk.y * step * float2(i.dd.w, 1.0);
    const float2 ext = 2.0 * sig + 0.5;
    static const float2 kQ[4] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };
    GaussOut o;
    o.col = i.col; o.da = float2(d, share);
    [unroll] for (int k = 0; k < 4; ++k)
    {
        const float2 at = p + kQ[k] * ext;
        o.pos = float4(at.x / g_size.x * 2.0 - 1.0, 1.0 - at.y / g_size.y * 2.0, 0.5, 1.0);
        o.off = kQ[k] * ext / sig;
        s.Append(o);
    }
    s.RestartStrip();
}
[maxvertexcount(8)]
void GSGauss(point GaussPt i[1], inout TriangleStream<GaussOut> s)
{
    if (i[0].keep > 0.5)
    {
        if (i[0].dd.z > 0.02) GaussQuad(i[0], i[0].dd.x, i[0].dd.z, s);
        if (i[0].dd.z < 0.98) GaussQuad(i[0], i[0].dd.y, 1.0 - i[0].dd.z, s);
    }
}
// (tex2: the new views' disparity, small. Added up -- near: colour by weight by share, and that weight;
// far: the same of what is behind; and apart, the near layer's weight without the share, and how much of the
// pixel is covered at all.)
struct GaussAcc { float4 near : SV_Target0; float4 far : SV_Target1; float2 wc : SV_Target2; };
GaussAcc PSGauss(GaussOut i)
{
    const int W = (int)g_size.x;
    const int pane = g_eye < 0.5 ? 0 : 1;
    const float2 full = float2(i.pos.x - (float)(pane * W), i.pos.y);
    const int w = (int)g_srcSize.x, h = (int)g_srcSize.y;
    const float dT = tex2.Load(int3(pane * w + clamp((int)(full.x / g_scale.x), 0, w - 1), clamp((int)(full.y / g_scale.y), 0, h - 1), 0)).r;
    const float g = exp(-0.5 * dot(i.off, i.off));
    const float wt = g * i.col.a;
    const float back = i.da.x - dT;
    GaussAcc o;
    o.near = 0; o.far = 0; o.wc = float2(0, g);
    if (back > -1.0) { o.near = float4(i.col.rgb, 1.0) * (wt * i.da.y); o.wc.x = wt; }
    // (Of what is behind, the nearer counts the more -- by degrees.)
    else o.far = float4(i.col.rgb, 1.0) * (wt * i.da.y * exp(1.5 * max(back, -6.0)));
    return o;
}
// (tex0: the near layer; tex1: the far; tex2: the near's whole weight and the cover. The near laid over the
// far by its share; with nothing behind it, whole. A pixel hardly any spot reached is not seen: PSUnseen's.)
float4 PSGaussResolve(VSOut i) : SV_Target
{
    const int3 p = int3((int)i.pos.x, (int)i.pos.y, 0);
    const float4 n = tex0.Load(p), b = tex1.Load(p);
    const float2 wc = tex2.Load(p).rg;
    const float3 cn = n.rgb / max(n.a, 1e-9), cb = b.rgb / max(b.a, 1e-9);
    float share = wc.x > 1e-7 ? saturate(n.a / wc.x) : 0.0;
    if (b.a < 1e-7) share = 1.0;
    if (n.a < 1e-9) share = 0.0;
    return float4(lerp(cb, cn, share), wc.y > g_lk.z ? 1.0 : 0.0);
}

// ---- Gaussian splats at the outlines only -----------------------------------
// Most of a picture is smooth surface, where a spot a pixel and a pixel looked
// up come to the same thing -- and the looking up is a tenth of the work. So:
// the new view is made the standard way, and spots are thrown only from where
// the map has an outline (the four texels round a place are not one surface),
// sixteen to such a cell (CSCells lists the cells, VSGaussCell makes the spots); they are then laid over the standard picture by how
// much of each pixel they cover.
// (A point a cell of the small map, both eyes' one after the other. tex0: the
// two eyes. tex1: their disparity, small.)
// (The cells that have an outline, listed: tex1 the disparity, small. One number a cell: its row times the
// map's whole width, and its column -- the right eye's after the left's.)
AppendStructuredBuffer<uint> g_cellsOut : register(u0);
[numthreads(8, 8, 1)]
void CSCells(uint3 id : SV_DispatchThreadID)
{
    const int w = (int)g_srcSize.x, h = (int)g_srcSize.y;
    const int X = (int)id.x, y0 = (int)id.y;
    const int ref = X >= w ? 1 : 0;
    const int x0 = X - ref * w;
    if (X < 2 * w && y0 < h - 1 && x0 < w - 1)
    {
        const float d00 = tex1.Load(int3(X, y0, 0)).r, d10 = tex1.Load(int3(X + 1, y0, 0)).r;
        const float d01 = tex1.Load(int3(X, y0 + 1, 0)).r, d11 = tex1.Load(int3(X + 1, y0 + 1, 0)).r;
        if (max(max(d00, d10), max(d01, d11)) - min(min(d00, d10), min(d01, d11)) >= 1.0) g_cellsOut.Append((uint)(y0 * 2 * w + X));
    }
}
// (tex0: the near layer; tex1: the far; tex2: the near's whole weight and the cover. Drawn ONTO the new view
// made the standard way, mixed in by the second colour's alpha (dual-source blending) -- so nothing is
// written, and next to nothing read, where no spot landed. The spots by how much of the pixel the NEAR layer
// covers: spots of what is behind the standard picture's own surface are only what a near spot's share is
// laid over, never drawn for themselves. A near spot's share is laid over the far spots where there are
// any, else over the standard picture. .a: seen, the larger of the two kept.)
struct MixOut { float4 c : SV_Target0; float4 f : SV_Target1; };
MixOut PSGaussMix(VSOut i)
{
    const int3 p = int3((int)i.pos.x, (int)i.pos.y, 0);
    const float2 wc = tex2.Load(p).rg;
    const float k = saturate((wc.x - 0.15) / 0.5);
    if (k <= 0.0) discard;
    const float4 n = tex0.Load(p), b = tex1.Load(p);
    if (n.a < 1e-9) discard;
    const float share = saturate(n.a / wc.x);
    const float3 cn = n.rgb / n.a;
    MixOut o;
    const bool behind = b.a > 1e-7;
    o.c = float4(behind ? lerp(b.rgb / b.a, cn, share) : cn, k > 0.5 ? 1.0 : 0.0);
    const float m = behind ? k : share * k;
    o.f = float4(m, m, m, m);
    return o;
}

// (The outlines' spots drawn straight onto the new view made the standard way, each over what is there by its
// own weight -- as Gaussian splats are commonly drawn, and with no buffers of their own to clear, add into and
// read back. Not sorted: they are all of the nearest surface -- what is behind it is left out -- and of one
// outline, so which goes over which changes little. tex2: the new views' disparity, small. .a: seen.)
MixOut PSGaussOver(GaussOut i)
{
    const int W = (int)g_size.x;
    const int pane = g_eye < 0.5 ? 0 : 1;
    const float2 full = float2(i.pos.x - (float)(pane * W), i.pos.y);
    const int w = (int)g_srcSize.x, h = (int)g_srcSize.y;
    const float dT = tex2.Load(int3(pane * w + clamp((int)(full.x / g_scale.x), 0, w - 1), clamp((int)(full.y / g_scale.y), 0, h - 1), 0)).r;
    if (i.da.x - dT <= -1.0) discard;
    const float a = exp(-0.5 * dot(i.off, i.off)) * i.da.y;
    if (a < 0.02) discard;
    MixOut o;
    o.c = float4(i.col.rgb, a > 0.5 ? 1.0 : 0.0);
    o.f = float4(a, a, a, a);
    return o;
}

// ---- Gaussian splats as points ------------------------------------------------
// The same spots, drawn the cheapest way there is: each a single point -- one vertex, one pixel -- which adds
// its colour and weight to the pixel its middle falls in, and with them how far from that pixel's own middle
// it really lies. Each pixel of the new view is then made from the nine round it: every one's spots taken as a
// Gaussian at the place they truly are, not at the pixel's middle. A sixth of the vertices of a quad a spot,
// and no small triangles; the cost is that all the spots of one pixel are spread from their mean place.
// (tex2: the new views' disparity, small.)
struct PointOut { float4 pos : SV_POSITION; nointerpolation float4 col : TEXCOORD0; nointerpolation float4 pda : TEXCOORD1; };
PointOut VSPoint(uint id : SV_VertexID)
{
    const GaussPt g = SpotOfId(id);
    const bool nearSide = g.dd.z >= 0.5;
    const float d = nearSide ? g.dd.x : g.dd.y;
    const float2 p = g.cm.xy - d * g.cm.zw * g_scale;
    PointOut o;
    o.pos = float4(p.x / g_size.x * 2.0 - 1.0, 1.0 - p.y / g_size.y * 2.0, 0.5, 1.0);
    if (g.keep < 0.5) o.pos = float4(2.0, 2.0, 2.0, 1.0);
    o.col = g.col;
    o.pda = float4(p, d, nearSide ? g.dd.z : 1.0 - g.dd.z);
    return o;
}
struct PointAcc { float4 sum : SV_Target0; float2 off : SV_Target1; };
PointAcc PSPoint(PointOut i)
{
    const int W = (int)g_size.x;
    const int pane = g_eye < 0.5 ? 0 : 1;
    const float2 full = float2(i.pos.x - (float)(pane * W), i.pos.y);
    const int w = (int)g_srcSize.x, h = (int)g_srcSize.y;
    const float dT = tex2.Load(int3(pane * w + clamp((int)(full.x / g_scale.x), 0, w - 1), clamp((int)(full.y / g_scale.y), 0, h - 1), 0)).r;
    if (i.pda.z - dT <= -1.0) discard;
    const float wt = i.col.a * i.pda.w;
    PointAcc o;
    o.sum = float4(i.col.rgb, 1.0) * wt;
    o.off = (i.pda.xy - full) * wt;
    return o;
}
// (tex0: colour by weight, and weight; tex1: the spots' mean place by weight. g_lk.y: a spot's width in pixels.)
float4 PSPointResolve(VSOut i) : SV_Target
{
    const int W = (int)g_size.x;
    const int2 p = int2((int)i.pos.x, (int)i.pos.y);
    const int pane = p.x >= W ? 1 : 0;
    const float inv = 0.5 / max(g_lk.y * g_lk.y * max(g_lk.x, 1.0) * max(g_lk.x, 1.0), 1e-4);
    float3 acc = 0; float wsum = 0, cover = 0;
    [unroll] for (int k = 0; k < 9; ++k)
    {
        const int2 n = int2(k % 3 - 1, k / 3 - 1);
        const int2 q = int2(clamp(p.x + n.x, pane * W, pane * W + W - 1), clamp(p.y + n.y, 0, (int)g_size.y - 1));
        const float4 a = tex0.Load(int3(q, 0));
        const float2 m = tex1.Load(int3(q, 0)).rg;
        const float2 r = float2(q - p) + m / max(a.a, 1e-9);
        const float g = a.a > 1e-9 ? exp(-dot(r, r) * inv) : 0.0;
        acc += a.rgb * g; wsum += a.a * g; cover += g;
    }
    return float4(acc / max(wsum, 1e-9), cover > g_lk.z ? 1.0 : 0.0);
}
