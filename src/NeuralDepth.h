#pragma once
// A neural stereo matcher for the look-around's still pictures (an experiment).
//
// The look-around's own matcher is quick and rough: against a measured truth a
// third to a half of a picture's disparities are out by more than two pixels,
// and it is the outlines that show it. A trained matcher is out on a twentieth
// to a fifth -- but takes most of a second a picture on a middling graphics
// card, where the look-around has a few thousandths. So it is for a picture
// that stays: once the converter's picture has stood still a moment it is handed
// over here, worked on in a thread of its own, and when the answer comes the
// look-around takes it in place of its own (LookAround::SetExternalDisparity).
// The picture changes: its own matcher again, at once.
//
// The model is not part of SR Loom: an ONNX file, <SRLoom.exe's folder>\neural\
// stereo.onnx, with two inputs (left_image, right_image: 1 x 3 x H x W, RGB,
// ImageNet-normalised) and one output (disparity: 1 x 1 x H x W, the left
// picture's, in pixels, never negative). Run through ONNX Runtime on DirectML;
// without onnxruntime.dll beside the exe, or without the model, none of this
// is there and nothing else is touched.

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
        bool Start(const std::wstring& exeDir, const std::wstring& modelFile = L"");   // (modelFile: a file in the neural folder beside the exe; none: Fast-FoundationStereo.onnx)
        void Shutdown();
        bool Ready() const { return m_ready.load(); }      // the model is loaded
        bool Busy() const { return m_busy.load(); }        // a picture is being worked on
        bool Failed() const { return m_failed.load(); }

        // A picture to work on: both eyes side by side, 8-bit RGBA rows (top
        // first, pitch bytes a row), each eye eyeW x eyeH. aspect: one eye's
        // true shape as shown, width over height (a half side-by-side's eye is
        // stored squeezed; the model must see it as it looks). mapW x mapH: the
        // size the answer is wanted at (LookAround::MapSize). serial: handed
        // back with the answer. False: busy, or not ready.
        bool Submit(const uint8_t* rgba, int pitch, int eyeW, int eyeH, float aspect, int mapW, int mapH, unsigned serial);

        // The answer, once: both eyes side by side at mapW x mapH each, four
        // half floats a texel -- .r the disparity in the map's texels, .g 1 --
        // as LookAround::SetExternalDisparity takes it. ms: how long the model took.
        bool Take(std::vector<uint16_t>& halfRGBA, int& mapW, int& mapH, unsigned& serial, double& ms);

    private:
        void Run();
        struct Job { std::vector<uint8_t> px; int eyeW = 0, eyeH = 0, mapW = 0, mapH = 0; float aspect = 1.0f; unsigned serial = 0; };

        std::wstring m_model;
        std::thread m_thread;
        std::mutex m_lock;
        std::condition_variable m_wake;
        bool m_quit = false, m_haveJob = false, m_haveResult = false;
        Job m_job;
        std::vector<uint16_t> m_result; int m_resW = 0, m_resH = 0; unsigned m_resSerial = 0; double m_resMs = 0.0;
        std::atomic<bool> m_ready{ false }, m_busy{ false }, m_failed{ false };
    };
}
