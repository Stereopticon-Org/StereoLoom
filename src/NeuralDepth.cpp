#include "NeuralDepth.h"
#include "Common.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
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
}

bool NeuralDepth::Start(const std::wstring& exeDir, const std::wstring& modelFile)
{
#ifdef SRLOOM_NEURAL
    if (m_thread.joinable()) return true;
    m_model = exeDir + L"\\neural\\" + (modelFile.empty() ? std::wstring(L"Fast-FoundationStereo.onnx") : modelFile);
    if (!Exists(exeDir + L"\\onnxruntime.dll") || !Exists(exeDir + L"\\DirectML.dll") || !Exists(m_model))
    {
        Log("NeuralDepth: not there (onnxruntime.dll, DirectML.dll beside the exe and neural\\stereo.onnx are all needed)");
        return false;
    }
    m_quit = false; m_failed = false;
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

bool NeuralDepth::Submit(const uint8_t* rgba, int pitch, int eyeW, int eyeH, float aspect, int mapW, int mapH, unsigned serial)
{
    if (!m_ready.load() || m_busy.load() || !rgba || eyeW < 64 || eyeH < 64 || mapW < 8 || mapH < 8) return false;
    {
        std::lock_guard<std::mutex> g(m_lock);
        if (m_haveJob) return false;
        m_job.px.resize((size_t)eyeW * 2 * 4 * eyeH);
        for (int y = 0; y < eyeH; ++y) memcpy(&m_job.px[(size_t)y * eyeW * 2 * 4], rgba + (size_t)y * pitch, (size_t)eyeW * 2 * 4);
        m_job.eyeW = eyeW; m_job.eyeH = eyeH; m_job.mapW = mapW; m_job.mapH = mapH; m_job.aspect = aspect; m_job.serial = serial;
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
        int fixW = 0, fixH = 0;
        {
            const std::vector<int64_t> s = session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
            if (s.size() == 4 && s[2] > 0 && s[3] > 0) { fixH = (int)s[2]; fixW = (int)s[3]; }
        }
        m_ready = true;
        Log("NeuralDepth: model loaded (%s)", fixW ? "a fixed size" : "any size");
        if (fixW) Log("NeuralDepth: the model runs at %dx%d", fixW, fixH);

        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> g(m_lock);
                m_wake.wait(g, [this] { return m_quit || m_haveJob; });
                if (m_quit) return;
                job = std::move(m_job); m_haveJob = false;
            }
            const auto t0 = std::chrono::steady_clock::now();
            // The size it is run at: 736 across (what it does best at), the
            // height by the picture's true shape, both in 32s.
            const int w = fixW ? fixW : 736;
            const int h = fixH ? fixH : (std::max)(64, (std::min)(1024, (int)std::lround((float)w / (std::max)(job.aspect, 0.25f) / 32.0f) * 32));
            // (A camera's picture has nothing behind the screen -- no point further
            // RIGHT in the right eye than in the left -- and the model was never
            // shown one that has. A game's has. So the other eye is slid by a
            // twentieth of the width first, and that is taken off the answer.)
            const int slide = (int)std::lround(0.05f * (float)w);
            std::vector<float> a, b, disp[2];
            const int64_t shape[4] = { 1, 3, h, w };
            for (int eye = 0; eye < 2; ++eye)
            {
                // (The right eye's: mirrored, the right eye is a left eye and the left its right.)
                Prepare(job.px, job.eyeW, job.eyeH, eye, w, h, eye == 1, 0, a);
                Prepare(job.px, job.eyeW, job.eyeH, 1 - eye, w, h, eye == 1, slide, b);
                Ort::Value in[2] = { Ort::Value::CreateTensor<float>(mem, a.data(), a.size(), shape, 4),
                                     Ort::Value::CreateTensor<float>(mem, b.data(), b.size(), shape, 4) };
                auto out = session.Run(Ort::RunOptions{ nullptr }, inNames, in, 2, outNames, 1);
                const float* d = out[0].GetTensorData<float>();
                disp[eye].resize((size_t)w * h);
                for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
                    disp[eye][(size_t)y * w + x] = d[(size_t)y * w + (eye == 1 ? w - 1 - x : x)] - (float)slide;
            }
            // To the map: bilinear, and from the model's pixels to the map's texels.
            std::vector<uint16_t> res((size_t)job.mapW * 2 * job.mapH * 4, 0);
            const float k = (float)job.mapW / (float)w;
            const uint16_t one = ToHalf(1.0f);
            for (int eye = 0; eye < 2; ++eye)
                for (int y = 0; y < job.mapH; ++y)
                {
                    const float fy = (std::max)(0.0f, (std::min)((float)h - 1.001f, ((float)y + 0.5f) * (float)h / (float)job.mapH - 0.5f));
                    const int y0 = (int)fy; const float wy = fy - (float)y0;
                    for (int x = 0; x < job.mapW; ++x)
                    {
                        const float fx = (std::max)(0.0f, (std::min)((float)w - 1.001f, ((float)x + 0.5f) * (float)w / (float)job.mapW - 0.5f));
                        const int x0 = (int)fx; const float wx = fx - (float)x0;
                        const float* p = &disp[eye][(size_t)y0 * w + x0];
                        const float v = (p[0] * (1 - wx) + p[1] * wx) * (1 - wy) + (p[w] * (1 - wx) + p[w + 1] * wx) * wy;
                        uint16_t* o = &res[((size_t)y * job.mapW * 2 + (size_t)eye * job.mapW + x) * 4];
                        o[0] = ToHalf(v * k); o[1] = one;
                    }
                }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            {
                std::lock_guard<std::mutex> g(m_lock);
                m_result.swap(res); m_resW = job.mapW; m_resH = job.mapH; m_resSerial = job.serial; m_resMs = ms; m_haveResult = true;
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
