#pragma once
// A trained stereo matcher for the look-around, and a painter of what is behind
// things (an experiment).
//
// The look-around's own matcher is quick and rough: against a measured truth a
// third to a half of a picture's disparities are out by more than two pixels,
// and it is the outlines that show it. A trained matcher is out on a tenth or
// so. The quick ones (Lite Any Stereo V2) answer in a tenth or a fifth of a
// second on a middling graphics card -- not a frame's time, so the work is done
// here in a thread of its own on a small copy of the picture, and the
// look-around takes each answer as it comes in place of its own
// (LookAround::SetExternalDisparity).
//
// And for a picture that stays, a background: the left eye with every near
// thing lifted off at its edges and what is behind it painted in by an
// inpainting network (LaMa; on the processor, a second or two), with that
// background's own depth carried across -- what the look-around then shows
// where neither eye sees (LookAround::SetLayer).
//
// The models are not part of SR Loom: ONNX files in <SRLoom.exe's folder>\neural
// -- a matcher with two inputs (1 x 3 x H x W, RGB, ImageNet-normalised) and one
// output (1 x 1 x H x W, the first picture's disparity in pixels, never
// negative); and neural\inpaint\lama.onnx (image 1 x 3 x 512 x 512 in 0..1 and
// mask 1 x 1 x 512 x 512; out 0..255). Run through ONNX Runtime; without
// onnxruntime.dll beside the exe, or without a model, none of this is there and
// nothing else is touched.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace srw
{
    class NeuralDepth
    {
    public:
        ~NeuralDepth() { Shutdown(); }

        // Begins loading the model (in the thread: it takes seconds). False: no
        // runtime or no model file -- not there at all.
        bool Start(const std::wstring& exeDir, const std::wstring& modelFile = L"");   // (modelFile: a file in the neural folder beside the exe; none: Lite Any Stereo M 512x288.onnx)
        void Shutdown();
        bool Ready() const { return m_ready.load(); }      // the model is loaded
        bool Busy() const { return m_busy.load(); }        // a picture is being worked on
        bool Failed() const { return m_failed.load(); }

        // The size the model wants its picture at (one eye): its own where it has
        // one fixed; else `width` across and the height by `aspect` (one eye's
        // true shape as shown, width over height), in 32s. Valid once Ready.
        void NetSize(float aspect, int width, int& w, int& h) const;

        // A picture to work on: both eyes side by side, 8-bit RGBA rows (top
        // first, pitch bytes a row), each eye eyeW x eyeH -- as a rule already at
        // NetSize (LookAround::Shrink), any size will do. aspect, width: as
        // NetSize. mapW x mapH: the size the answer is wanted at
        // (LookAround::MapSize). serial: handed back with the answer. False:
        // busy, or not ready.
        bool Submit(const uint8_t* rgba, int pitch, int eyeW, int eyeH, float aspect, int width, int mapW, int mapH, unsigned serial);

        // The answer, once: both eyes side by side at mapW x mapH each, four
        // half floats a texel -- .r the disparity in the map's texels, .g 1 --
        // as LookAround::SetExternalDisparity takes it. ms: how long the model took.
        bool Take(std::vector<uint16_t>& halfRGBA, int& mapW, int& mapH, unsigned& serial, double& ms);

        // The painted background for the picture last answered (serial must be
        // its). Asked for once the picture has stayed; worked on when the thread
        // is free. False: no painter, busy with one, or that picture has gone.
        // The left eye only (the caller makes the right from it): half the work.
        void SetLeftOnly(bool on) { m_leftOnly = on; }
        // Where the time goes, smoothed, in ms: making the inputs, the model itself, the whole of an answer.
        void Times(double& prep, double& run, double& all) { std::lock_guard<std::mutex> g(m_lock); prep = m_tPrep; run = m_tRun; all = m_tAll; }
        bool PainterThere() const { return m_painter; }
        // Every picture the matcher answers painted, when the painter is free for it (a moving picture's too).
        void SetAutoPaint(bool on) { m_autoPaint = on; }
        bool RequestLayer(unsigned serial, int mapW);
        // ... and taken, once: n x n texels of four floats -- red, green, blue
        // linear, and the background's disparity in the map's texels -- as
        // LookAround::SetLayer takes it.
        bool TakeLayer(std::vector<float>& rgba, int& n, unsigned& serial, double& ms);

        // The matcher's last answered picture, as the painter is given it.
        struct Shot
        {
            std::vector<uint8_t> px; int eyeW = 0, eyeH = 0;        // both eyes side by side, RGBA
            std::vector<float> disp[2]; int w = 0, h = 0;           // each eye's disparity, in the model's pixels (the right may be empty)
            unsigned serial = 0; bool have = false;
        };

    private:
        void PaintRun();
        void Run();
        struct Job { std::vector<uint8_t> px; int eyeW = 0, eyeH = 0, mapW = 0, mapH = 0, width = 736; float aspect = 1.0f; unsigned serial = 0; };

        std::wstring m_model, m_paintModel;
        bool m_painter = false, m_paintGpu = false;
        std::thread m_thread;
        std::thread m_paintThread; std::mutex m_paintLock; std::condition_variable m_paintWake;   // (the painter's: m_shot, the layer and what is asked of it are under m_paintLock)
        Shot m_shot; bool m_shotNew = false; int m_shotMapW = 0; std::atomic<bool> m_autoPaint{ false };
        std::mutex m_lock;
        std::condition_variable m_wake;
        bool m_quit = false, m_haveJob = false, m_haveResult = false;
        Job m_job;
        std::vector<uint16_t> m_result; int m_resW = 0, m_resH = 0; unsigned m_resSerial = 0; double m_resMs = 0.0;
        bool m_wantLayer = false, m_haveLayer = false; unsigned m_layerSerial = 0; int m_layerMapW = 0;
        std::vector<float> m_layer; int m_layerN = 0; unsigned m_layerDoneSerial = 0; double m_layerMs = 0.0;
        std::atomic<bool> m_ready{ false }, m_busy{ false }, m_failed{ false };
        std::atomic<int> m_fixW{ 0 }, m_fixH{ 0 };
        std::atomic<bool> m_leftOnly{ false };
        double m_tPrep = 0.0, m_tRun = 0.0, m_tAll = 0.0;   // (smoothed, ms: making the inputs, the model, all of it)
    };
}
