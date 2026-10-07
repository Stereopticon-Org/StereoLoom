#include "NeuralDepth.h"
#include "Common.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

#ifdef SRLOOM_NEURAL
// (onnxruntime.dll is delay-loaded: its API is fetched only once the DLL is
// known to be there, so SR Loom starts the same without it.)
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#endif

using namespace srw;

namespace
{
    bool Exists(const std::wstring& p) { const DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

    uint16_t ToHalf(float v)
    {
        uint32_t x; memcpy(&x, &v, 4);
        const uint32_t sign = (x >> 16) & 0x8000; const int e = (int)((x >> 23) & 0xff) - 127 + 15; uint32_t m = x & 0x7fffff;
        if (e <= 0) { if (e < -10) return (uint16_t)sign; m = (m | 0x800000) >> (1 - e); return (uint16_t)(sign | (m >> 13)); }
        if (e >= 31) return (uint16_t)(sign | 0x7c00);
        return (uint16_t)(sign | (e << 10) | (m >> 13));
    }

    // One eye of the side-by-side picture as the model's input: w x h, planes R
    // G B, ImageNet-normalised; each of its pixels the mean of the picture's
    // pixels it covers. mirror: left to right. slide: its content that many of
    // ITS pixels to the left (the edge repeated).
    void Prepare(const std::vector<uint8_t>& px, int eyeW, int eyeH, int eye, int w, int h, bool mirror, int slide, std::vector<float>& out)
    {
        static const float mean[3] = { 0.485f, 0.456f, 0.406f }, dev[3] = { 0.229f, 0.224f, 0.225f };
        out.resize((size_t)3 * w * h);
        const size_t pitch = (size_t)eyeW * 2 * 4;
        for (int y = 0; y < h; ++y)
        {
            const int y0 = y * eyeH / h, y1 = (std::max)(y0 + 1, (y + 1) * eyeH / h);
            for (int x = 0; x < w; ++x)
            {
                int xs = (std::min)(w - 1, x + slide);           // (which of the model's columns this one shows)
                if (mirror) xs = w - 1 - xs;
                const int x0 = xs * eyeW / w, x1 = (std::max)(x0 + 1, (xs + 1) * eyeW / w);
                unsigned s[3] = {}; unsigned n = 0;
                for (int yy = y0; yy < y1; ++yy)
                {
                    const uint8_t* row = px.data() + (size_t)yy * pitch + ((size_t)eye * eyeW + x0) * 4;
                    for (int xx = x0; xx < x1; ++xx, row += 4) { s[0] += row[0]; s[1] += row[1]; s[2] += row[2]; ++n; }
                }
                for (int c = 0; c < 3; ++c) out[(size_t)c * w * h + (size_t)y * w + x] = ((float)s[c] / (float)n / 255.0f - mean[c]) / dev[c];
            }
        }
    }

    // What the thread keeps of the picture last answered, for the painter.
    struct Last
    {
        std::vector<uint8_t> px; int eyeW = 0, eyeH = 0;        // both eyes side by side, RGBA
        std::vector<float> disp[2]; int w = 0, h = 0;           // each eye's disparity, in the model's pixels
        unsigned serial = 0; bool have = false;
    };

    float Bilinear(const std::vector<float>& a, int w, int h, float x, float y)
    {
        x = (std::max)(0.0f, (std::min)((float)w - 1.001f, x)); y = (std::max)(0.0f, (std::min)((float)h - 1.001f, y));
        const int x0 = (int)x, y0 = (int)y; const float fx = x - (float)x0, fy = y - (float)y0;
        const float* p = &a[(size_t)y0 * w + x0];
        return (p[0] * (1 - fx) + p[1] * fx) * (1 - fy) + (p[w] * (1 - fx) + p[w + 1] * fx) * fy;
    }

    // The largest (or the smallest) within r texels, along rows then columns.
    void Spread(std::vector<float>& a, int n, int r, bool largest)
    {
        std::vector<float> t((size_t)n * n);
        for (int pass = 0; pass < 2; ++pass)
        {
            for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x)
            {
                float v = a[(size_t)y * n + x];
                for (int k = -r; k <= r; ++k)
                {
                    const int q = pass == 0 ? x + k : y + k;
                    if (q < 0 || q >= n) continue;
                    const float u = pass == 0 ? a[(size_t)y * n + q] : a[(size_t)q * n + x];
                    v = largest ? (std::max)(v, u) : (std::min)(v, u);
                }
                t[(size_t)y * n + x] = v;
            }
            a.swap(t);
        }
    }

    // Values spread into where there are none (wgt 0) from where there are: made
    // smaller and smaller, each texel the mean of those of its four that have a
    // value, and then brought back up -- a gap takes the coarsest level's word
    // only where no finer one has any.
    void PushPull(std::vector<float>& val, std::vector<float>& wgt, int n)
    {
        if (n <= 1) return;
        const int m = n / 2;
        std::vector<float> v2((size_t)m * m, 0.0f), w2((size_t)m * m, 0.0f);
        for (int y = 0; y < m; ++y) for (int x = 0; x < m; ++x)
        {
            float s = 0, w = 0;
            for (int k = 0; k < 4; ++k) { const size_t i = (size_t)(y * 2 + (k >> 1)) * n + (x * 2 + (k & 1)); s += val[i] * wgt[i]; w += wgt[i]; }
            if (w > 0) { v2[(size_t)y * m + x] = s / w; w2[(size_t)y * m + x] = 1.0f; }
        }
        PushPull(v2, w2, m);
        for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x)
        {
            const size_t i = (size_t)y * n + x;
            if (wgt[i] > 0) continue;
            val[i] = Bilinear(v2, m, m, ((float)x + 0.5f) * 0.5f - 0.5f, ((float)y + 0.5f) * 0.5f - 0.5f);
            wgt[i] = 1.0f;
        }
    }

    float ToLinear(float s) { return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f); }

#ifdef SRLOOM_NEURAL
    // The painted background: see NeuralDepth.h. n x n, four floats a texel.
    void BuildLayer(const Last& last, Ort::Session& lama, int mapW, std::vector<float>& out, int& outN)
    {
        const int N = 512; outN = N;
        const size_t NN = (size_t)N * N;
        const float kd = (float)N / (float)last.w;                   // the model's pixels to the layer's
        // Both eyes and their disparities at the layer's size.
        std::vector<float> img[2], ds[2];
        for (int e = 0; e < 2; ++e)
        {
            img[e].resize(NN * 3); ds[e].resize(NN);
            const size_t pitch = (size_t)last.eyeW * 2 * 4;
            for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x)
            {
                const int x0 = x * last.eyeW / N, x1 = (std::max)(x0 + 1, (x + 1) * last.eyeW / N), y0 = y * last.eyeH / N, y1 = (std::max)(y0 + 1, (y + 1) * last.eyeH / N);
                unsigned s[3] = {}, n = 0;
                for (int yy = y0; yy < y1; ++yy) { const uint8_t* row = last.px.data() + (size_t)yy * pitch + ((size_t)e * last.eyeW + x0) * 4; for (int xx = x0; xx < x1; ++xx, row += 4) { s[0] += row[0]; s[1] += row[1]; s[2] += row[2]; ++n; } }
                for (int c = 0; c < 3; ++c) img[e][((size_t)y * N + x) * 3 + c] = (float)s[c] / (float)n / 255.0f;
                ds[e][(size_t)y * N + x] = last.disp[e].empty() ? 0.0f : kd * Bilinear(last.disp[e], last.w, last.h, ((float)x + 0.5f) * (float)last.w / (float)N - 0.5f, ((float)y + 0.5f) * (float)last.h / (float)N - 0.5f);
            }
        }
        // (The right eye's disparity, where the matcher was asked for the left's only: each left texel thrown to
        // where the right eye has it, the nearest kept, and what nothing lands on given the farther side's.)
        if (last.disp[1].empty())
        {
            std::vector<float>& r = ds[1];
            std::fill(r.begin(), r.end(), -1e9f);
            for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x)
            {
                const float v = ds[0][(size_t)y * N + x]; const int xr = (int)std::lround((float)x - v);
                if (xr >= 0 && xr < N && v > r[(size_t)y * N + xr]) r[(size_t)y * N + xr] = v;
            }
            for (int y = 0; y < N; ++y)
            {
                float* row = &r[(size_t)y * N]; float prev = -1e9f;
                std::vector<float> fromLeft(N);
                for (int x = 0; x < N; ++x) { if (row[x] > -1e8f) prev = row[x]; fromLeft[x] = prev; }
                float next = -1e9f;
                for (int x = N - 1; x >= 0; --x)
                {
                    if (row[x] > -1e8f) { next = row[x]; continue; }
                    const float a = fromLeft[x], b = next;
                    row[x] = (a > -1e8f && b > -1e8f) ? (std::min)(a, b) : a > -1e8f ? a : b > -1e8f ? b : 0.0f;
                }
            }
        }
        const std::vector<float>& d = ds[0];
        // A near thing's edge: a texel nearer than a neighbour by a step. Within
        // R of such an edge, and nearer than the farthest within R, is lifted off.
        const int R = 40;
        std::vector<float> reach(NN, 0.0f), farthest = d, band(NN, 0.0f);
        for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x)
        {
            const float v = d[(size_t)y * N + x];
            const bool stepped = (x > 0 && v - d[(size_t)y * N + x - 1] > 2.0f) || (x < N - 1 && v - d[(size_t)y * N + x + 1] > 2.0f) ||
                                 (y > 0 && v - d[(size_t)(y - 1) * N + x] > 2.0f) || (y < N - 1 && v - d[(size_t)(y + 1) * N + x] > 2.0f);
            reach[(size_t)y * N + x] = stepped ? 1.0f : 0.0f;
            // (How far in from this edge the head's moving can uncover: the step's own size, and a little.)
            float step = 0.0f;
            if (x > 0) step = (std::max)(step, v - d[(size_t)y * N + x - 1]); if (x < N - 1) step = (std::max)(step, v - d[(size_t)y * N + x + 1]);
            if (y > 0) step = (std::max)(step, v - d[(size_t)(y - 1) * N + x]); if (y < N - 1) step = (std::max)(step, v - d[(size_t)(y + 1) * N + x]);
            band[(size_t)y * N + x] = stepped ? (std::min)(40.0f, step * 1.3f + 4.0f) : 0.0f;
        }
        Spread(reach, N, R, true);
        Spread(farthest, N, R, false);
        std::vector<float> mask(NN);
        for (size_t i = 0; i < NN; ++i) mask[i] = (reach[i] > 0.5f && d[i] - farthest[i] > 1.5f) ? 1.0f : 0.0f;
        Spread(mask, N, 2, true);                                    // (a little wider: the edge's own mixed texels)
        // (The band: each texel's count of how much further in it reaches, one less a texel.)
        for (int it = 0; it < 40; ++it)
        {
            bool any = false;
            for (int pass = 0; pass < 2; ++pass)
                for (int yy = 0; yy < N; ++yy) for (int xx = 0; xx < N; ++xx)
                {
                    const int y = pass == 0 ? yy : N - 1 - yy, x = pass == 0 ? xx : N - 1 - xx;
                    float& b = band[(size_t)y * N + x]; float best = b;
                    if (x > 0) best = (std::max)(best, band[(size_t)y * N + x - 1] - 1.0f); if (x < N - 1) best = (std::max)(best, band[(size_t)y * N + x + 1] - 1.0f);
                    if (y > 0) best = (std::max)(best, band[(size_t)(y - 1) * N + x] - 1.0f); if (y < N - 1) best = (std::max)(best, band[(size_t)(y + 1) * N + x] - 1.0f);
                    if (best > b) { b = best; any = true; }
                }
            if (!any) break;
        }
        // The background's depth across what was lifted off: from what is known
        // to be background beside it -- not from the near thing's own inside.
        std::vector<float> bg = d, known(NN);
        // (Known: all that was not lifted off. Asking as well that it be the farthest thing near by threw out
        // sloping ground -- always nearer than the ground further off -- and what was left to spread from was the
        // far water, across everything.)
        for (size_t i = 0; i < NN; ++i) known[i] = mask[i] < 0.5f ? 1.0f : 0.0f;
        {
            std::vector<float> v = d, w = known;
            PushPull(v, w, N);
            for (size_t i = 0; i < NN; ++i) if (mask[i] > 0.5f) bg[i] = (std::min)(v[i], d[i]);
        }
        // What is behind a near thing as the left eye has it is, in part, in
        // plain sight of the right: that is put in as it is, and only what
        // neither eye has is left to be painted.
        // Painted is the band alone -- the strip along each edge that moving the head can uncover. The
        // rest of what was lifted off, which is never seen, is first given the background's colours
        // spread in from beside it: so the painter carries on from the ground a thing stands on, and
        // not from the thing, nor from what is on its other side.
        std::vector<float> paint = img[0], pm(NN);
        for (int c = 0; c < 3; ++c)
        {
            std::vector<float> v(NN), w = known;
            for (size_t i = 0; i < NN; ++i) v[i] = img[0][i * 3 + c];
            PushPull(v, w, N);
            for (size_t i = 0; i < NN; ++i) if (mask[i] > 0.5f) paint[i * 3 + c] = v[i];
        }
        for (size_t i = 0; i < NN; ++i) pm[i] = (mask[i] > 0.5f && band[i] > 0.0f) ? 1.0f : 0.0f;
        for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x)
        {
            const size_t i = (size_t)y * N + x;
            if (mask[i] < 0.5f) continue;
            const float xr = (float)x - bg[i];
            if (xr < 0.0f || xr > (float)N - 1.0f) continue;
            if (std::fabs(Bilinear(ds[1], N, N, xr, (float)y) - bg[i]) >= 1.5f) continue;
            const int x0 = (std::min)(N - 2, (int)xr); const float fx = xr - (float)x0;
            for (int c = 0; c < 3; ++c) paint[i * 3 + c] = img[1][((size_t)y * N + x0) * 3 + c] * (1 - fx) + img[1][((size_t)y * N + x0 + 1) * 3 + c] * fx;
            pm[i] = 0.0f;
        }
        // The painting.
        {
            Ort::AllocatorWithDefaultOptions alloc;
            const Ort::AllocatedStringPtr in0 = lama.GetInputNameAllocated(0, alloc), in1 = lama.GetInputNameAllocated(1, alloc), out0 = lama.GetOutputNameAllocated(0, alloc);
            const char* inNames[2] = { in0.get(), in1.get() }; const char* outNames[1] = { out0.get() };
            // (Two painters' ways: LaMa takes the picture in 0..1 and a mask of 1 where it is to paint, and
            // answers in 0..255; MI-GAN takes and gives bytes, its mask 255 where the picture is KNOWN.)
            const bool bytes = lama.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;
            const Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            const int64_t s3[4] = { 1, 3, N, N }, s1[4] = { 1, 1, N, N };
            if (bytes)
            {
                std::vector<uint8_t> pb(NN * 3), mb(NN);
                for (size_t i = 0; i < NN; ++i) { for (int c = 0; c < 3; ++c) pb[(size_t)c * NN + i] = (uint8_t)std::lround((std::max)(0.0f, (std::min)(1.0f, paint[i * 3 + c])) * 255.0f); mb[i] = pm[i] > 0.5f ? 0 : 255; }
                Ort::Value in[2] = { Ort::Value::CreateTensor<uint8_t>(mem, pb.data(), pb.size(), s3, 4), Ort::Value::CreateTensor<uint8_t>(mem, mb.data(), mb.size(), s1, 4) };
                auto res = lama.Run(Ort::RunOptions{ nullptr }, inNames, in, 2, outNames, 1);
                const uint8_t* o = res[0].GetTensorData<uint8_t>();
                for (size_t i = 0; i < NN; ++i) if (pm[i] > 0.5f) for (int c = 0; c < 3; ++c) paint[i * 3 + c] = (float)o[(size_t)c * NN + i] / 255.0f;
            }
            else
            {
            std::vector<float> planes(NN * 3);
            for (size_t i = 0; i < NN; ++i) for (int c = 0; c < 3; ++c) planes[(size_t)c * NN + i] = paint[i * 3 + c];
            Ort::Value in[2] = { Ort::Value::CreateTensor<float>(mem, planes.data(), planes.size(), s3, 4), Ort::Value::CreateTensor<float>(mem, pm.data(), pm.size(), s1, 4) };
            auto res = lama.Run(Ort::RunOptions{ nullptr }, inNames, in, 2, outNames, 1);
            const float* o = res[0].GetTensorData<float>();
            for (size_t i = 0; i < NN; ++i) if (pm[i] > 0.5f) for (int c = 0; c < 3; ++c) paint[i * 3 + c] = (std::max)(0.0f, (std::min)(1.0f, o[(size_t)c * NN + i] / 255.0f));
            }
        }
        out.resize(NN * 4);
        const float km = (float)mapW / (float)N;                     // the layer's pixels to the map's texels
        for (size_t i = 0; i < NN; ++i)
        {
            for (int c = 0; c < 3; ++c) out[i * 4 + c] = ToLinear(paint[i * 3 + c]);
            out[i * 4 + 3] = bg[i] * km;
        }
    }
#endif
}

bool NeuralDepth::Start(const std::wstring& exeDir, const std::wstring& modelFile)
{
#ifdef SRLOOM_NEURAL
    if (m_thread.joinable()) return true;
    m_model = exeDir + L"\\neural\\" + (modelFile.empty() ? std::wstring(L"Lite Any Stereo M 512x288.onnx") : modelFile);
    // (A model chosen once and since taken away: the usual one.)
    if (!Exists(m_model)) m_model = exeDir + L"\\neural\\Lite Any Stereo M 512x288.onnx";
    if (!Exists(exeDir + L"\\onnxruntime.dll") || !Exists(exeDir + L"\\DirectML.dll") || !Exists(m_model))
    {
        Log("NeuralDepth: not there (onnxruntime.dll and DirectML.dll beside the exe, and a model in the neural folder, are all needed)");
        return false;
    }
    // (The painter: MI-GAN where it is there -- it runs on the graphics card, in a thirtieth of a second -- else
    // LaMa, on the processor, in a second or two. SRLOOM_PAINTER=lama: LaMa though both are there.)
    m_paintModel = exeDir + L"\\neural\\inpaint\\migan.onnx"; m_paintGpu = true;
    {
        wchar_t pick[16] = {};
        const bool wantLama = GetEnvironmentVariableW(L"SRLOOM_PAINTER", pick, 16) > 0 && !_wcsicmp(pick, L"lama");
        if (wantLama || !Exists(m_paintModel)) { m_paintModel = exeDir + L"\\neural\\inpaint\\lama.onnx"; m_paintGpu = false; }
    }
    m_painter = Exists(m_paintModel);
    m_quit = false; m_failed = false; m_fixW = 0; m_fixH = 0;
    m_haveJob = false; m_haveResult = false; m_wantLayer = false; m_haveLayer = false;
    m_thread = std::thread([this] { Run(); });
    return true;
#else
    (void)exeDir; (void)modelFile;
    return false;
#endif
}

void NeuralDepth::Shutdown()
{
    {
        std::lock_guard<std::mutex> g(m_lock);
        m_quit = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
    m_ready = false; m_busy = false;
}

void NeuralDepth::NetSize(float aspect, int width, int& w, int& h) const
{
    const int fw = m_fixW.load(), fh = m_fixH.load();
    if (fw > 0 && fh > 0) { w = fw; h = fh; return; }
    w = (std::max)(256, (std::min)(1920, width / 32 * 32));
    h = (std::max)(64, (std::min)(1088, (int)std::lround((float)w / (std::max)(aspect, 0.25f) / 32.0f) * 32));
}

bool NeuralDepth::Submit(const uint8_t* rgba, int pitch, int eyeW, int eyeH, float aspect, int width, int mapW, int mapH, unsigned serial)
{
    if (!m_ready.load() || m_busy.load() || !rgba || eyeW < 64 || eyeH < 64 || mapW < 8 || mapH < 8) return false;
    {
        std::lock_guard<std::mutex> g(m_lock);
        if (m_haveJob) return false;
        m_job.px.resize((size_t)eyeW * 2 * 4 * eyeH);
        for (int y = 0; y < eyeH; ++y) memcpy(&m_job.px[(size_t)y * eyeW * 2 * 4], rgba + (size_t)y * pitch, (size_t)eyeW * 2 * 4);
        m_job.eyeW = eyeW; m_job.eyeH = eyeH; m_job.mapW = mapW; m_job.mapH = mapH; m_job.aspect = aspect; m_job.width = width; m_job.serial = serial;
        m_haveJob = true; m_busy = true;
    }
    m_wake.notify_all();
    return true;
}

bool NeuralDepth::Take(std::vector<uint16_t>& halfRGBA, int& mapW, int& mapH, unsigned& serial, double& ms)
{
    std::lock_guard<std::mutex> g(m_lock);
    if (!m_haveResult) return false;
    halfRGBA.swap(m_result); mapW = m_resW; mapH = m_resH; serial = m_resSerial; ms = m_resMs;
    m_haveResult = false;
    return true;
}

bool NeuralDepth::RequestLayer(unsigned serial, int mapW)
{
    if (!m_painter || !m_ready.load() || mapW < 8) return false;
    {
        std::lock_guard<std::mutex> g(m_lock);
        if (m_wantLayer) return false;
        m_wantLayer = true; m_layerSerial = serial; m_layerMapW = mapW;
    }
    m_wake.notify_all();
    return true;
}

bool NeuralDepth::TakeLayer(std::vector<float>& rgba, int& n, unsigned& serial, double& ms)
{
    std::lock_guard<std::mutex> g(m_lock);
    if (!m_haveLayer) return false;
    rgba.swap(m_layer); n = m_layerN; serial = m_layerDoneSerial; ms = m_layerMs;
    m_haveLayer = false;
    return true;
}

void NeuralDepth::Run()
{
#ifdef SRLOOM_NEURAL
    try
    {
        Ort::InitApi();
        Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "srloom");
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        so.DisableMemPattern();                               // (DirectML asks for both)
        so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(so, 0));
        Ort::Session session(env, m_model.c_str(), so);
        const Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        // (The inputs and the output by the model's own names; and its size, where it has one fixed.)
        Ort::AllocatorWithDefaultOptions alloc;
        if (session.GetInputCount() < 2 || session.GetOutputCount() < 1) throw std::runtime_error("the model has not two inputs and an output");
        const Ort::AllocatedStringPtr in0 = session.GetInputNameAllocated(0, alloc), in1 = session.GetInputNameAllocated(1, alloc), out0 = session.GetOutputNameAllocated(0, alloc);
        const char* inNames[2] = { in0.get(), in1.get() };
        const char* outNames[1] = { out0.get() };
        {
            const std::vector<int64_t> s = session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
            if (s.size() == 4 && s[2] > 0 && s[3] > 0) { m_fixH = (int)s[2]; m_fixW = (int)s[3]; }
        }
        m_ready = true;
        if (m_fixW.load()) Log("NeuralDepth: model loaded; it runs at %dx%d", m_fixW.load(), m_fixH.load());
        else               Log("NeuralDepth: model loaded; it runs at any size");

        // (The painter: on the processor -- it does not run on DirectML -- and made when first asked for.)
        std::unique_ptr<Ort::Session> lama;
        Last last;

        for (;;)
        {
            Job job; bool doJob = false, doLayer = false; unsigned layerSerial = 0; int layerMapW = 0;
            {
                std::unique_lock<std::mutex> g(m_lock);
                m_wake.wait(g, [this] { return m_quit || m_haveJob || m_wantLayer; });
                if (m_quit) return;
                if (m_haveJob) { job = std::move(m_job); m_haveJob = false; doJob = true; }
                else { doLayer = true; layerSerial = m_layerSerial; layerMapW = m_layerMapW; }
            }
            if (doLayer)
            {
                const auto t0 = std::chrono::steady_clock::now();
                std::vector<float> layer; int n = 0; bool made = false;
                if (last.have && last.serial == layerSerial)
                {
                    try
                    {
                        if (!lama)
                        {
                            Ort::SessionOptions cpu;
                            if (m_paintGpu) { cpu.DisableMemPattern(); cpu.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL); Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(cpu, 0)); }
                            cpu.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
                            cpu.SetIntraOpNumThreads((int)(std::max)(2u, std::thread::hardware_concurrency() / 2));
                            lama = std::make_unique<Ort::Session>(env, m_paintModel.c_str(), cpu);
                            Log("NeuralDepth: painter loaded (%s)", m_paintGpu ? "MI-GAN, on the graphics card" : "LaMa, on the processor");
                        }
                        BuildLayer(last, *lama, layerMapW, layer, n);
                        made = true;
                    }
                    catch (const std::exception& e) { Log("NeuralDepth: the painter failed: %s", e.what()); m_painter = false; }
                }
                {
                    std::lock_guard<std::mutex> g(m_lock);
                    m_wantLayer = false;
                    if (made) { m_layer.swap(layer); m_layerN = n; m_layerDoneSerial = layerSerial; m_haveLayer = true;
                                m_layerMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
                }
                continue;
            }
            const auto t0 = std::chrono::steady_clock::now();
            int w = 0, h = 0; NetSize(job.aspect, job.width, w, h);
            // (A camera's picture has nothing behind the screen -- no point further
            // RIGHT in the right eye than in the left -- and the model was never
            // shown one that has. A game's has. So the other eye is slid by a
            // twentieth of the width first -- no more than 56 pixels: the model
            // looks no further than 192 -- and that is taken off the answer.)
            const int slide = (std::min)(56, (int)std::lround(0.05f * (float)w));
            std::vector<float> a, b, disp[2];
            const int64_t shape[4] = { 1, 3, h, w };
            // (Left only: the mirrored run for the right eye is not made -- the look-around makes that map from
            // the left's, and the work here is halved.)
            const int eyes = m_leftOnly.load() ? 1 : 2;
            double tPrep = 0.0, tRun = 0.0;
            for (int eye = 0; eye < eyes; ++eye)
            {
                // (The right eye's: mirrored, the right eye is a left eye and the left its right.)
                const auto p0 = std::chrono::steady_clock::now();
                Prepare(job.px, job.eyeW, job.eyeH, eye, w, h, eye == 1, 0, a);
                Prepare(job.px, job.eyeW, job.eyeH, 1 - eye, w, h, eye == 1, slide, b);
                const auto p1 = std::chrono::steady_clock::now();
                Ort::Value in[2] = { Ort::Value::CreateTensor<float>(mem, a.data(), a.size(), shape, 4),
                                     Ort::Value::CreateTensor<float>(mem, b.data(), b.size(), shape, 4) };
                auto out = session.Run(Ort::RunOptions{ nullptr }, inNames, in, 2, outNames, 1);
                const auto p2 = std::chrono::steady_clock::now();
                tPrep += std::chrono::duration<double, std::milli>(p1 - p0).count(); tRun += std::chrono::duration<double, std::milli>(p2 - p1).count();
                const float* d = out[0].GetTensorData<float>();
                disp[eye].resize((size_t)w * h);
                for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
                    disp[eye][(size_t)y * w + x] = d[(size_t)y * w + (eye == 1 ? w - 1 - x : x)] - (float)slide;
            }
            // To the map: bilinear, and from the model's pixels to the map's texels.
            std::vector<uint16_t> res((size_t)job.mapW * 2 * job.mapH * 4, 0);
            const float k = (float)job.mapW / (float)w;
            const uint16_t one = ToHalf(1.0f);
            for (int eye = 0; eye < eyes; ++eye)
                for (int y = 0; y < job.mapH; ++y)
                    for (int x = 0; x < job.mapW; ++x)
                    {
                        const float v = Bilinear(disp[eye], w, h, ((float)x + 0.5f) * (float)w / (float)job.mapW - 0.5f, ((float)y + 0.5f) * (float)h / (float)job.mapH - 0.5f);
                        uint16_t* o = &res[((size_t)y * job.mapW * 2 + (size_t)eye * job.mapW + x) * 4];
                        o[0] = ToHalf(v * k); o[1] = one;
                    }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            // (Kept for the painter, should this picture stay.)
            if (m_painter)
            {
                last.px.swap(job.px); last.eyeW = job.eyeW; last.eyeH = job.eyeH; last.disp[0].swap(disp[0]); last.disp[1].swap(disp[1]);
                last.w = w; last.h = h; last.serial = job.serial; last.have = true;
            }
            {
                std::lock_guard<std::mutex> g(m_lock);
                m_result.swap(res); m_resW = job.mapW; m_resH = job.mapH; m_resSerial = job.serial; m_resMs = ms; m_haveResult = true;
                m_tPrep += (tPrep - m_tPrep) * 0.2; m_tRun += (tRun - m_tRun) * 0.2; m_tAll += (ms - m_tAll) * 0.2;
            }
            m_busy = false;
        }
    }
    catch (const std::exception& e)
    {
        Log("NeuralDepth: failed: %s", e.what());
    }
    catch (...)
    {
        Log("NeuralDepth: failed");
    }
    m_failed = true; m_ready = false; m_busy = false;
#endif
}
