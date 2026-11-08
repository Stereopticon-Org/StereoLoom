// KatangaOutput.h -- the Katanga output: the two eyes handed to Katanga (bo3b's
// open-source stereo viewer, github.com/bo3b/Katanga) as a game would hand them, so
// Katanga shows them in a VR headset. Katanga's shared-texture protocol, as wiz3D
// uses it: Katanga, running first, owns the mutex "KatangaSetupMutex" and the
// mapped file "Local\KatangaMappedFile"; the producer puts the legacy shared handle
// of a 2W x H side-by-side texture in the mapped file and, under the mutex, copies
// each frame into that texture. (Stereo Loom's own Katanga receiver reads the same
// file: it stands aside while this output is on.)
#pragma once

#include "Common.h"
#include <d3d11.h>

namespace srw
{
    class KatangaOutput
    {
    public:
        ~KatangaOutput() { Stop(); }
        // Katanga is running (its mutex is there).
        static bool KatangaRunning();
        bool Start();                 // false: Katanga is not running
        void Stop();
        bool IsUp() const { return m_mutex != nullptr; }
        // Where to draw this frame's side by side (2 * eyeW x eyeH), made or
        // remade at that size; then Publish.
        ID3D11RenderTargetView* Target(ID3D11Device* device, int eyeW, int eyeH);
        void Publish(ID3D11DeviceContext* ctx);
        int Width() const { return m_w; }     // (the whole side by side)
        int Height() const { return m_h; }

    private:
        void ReleaseTextures();
        HANDLE                  m_mutex = nullptr, m_file = nullptr;
        void*                   m_view = nullptr;
        ID3D11Texture2D*        m_draw = nullptr;     // (drawn into)
        ID3D11RenderTargetView* m_rtv = nullptr;
        ID3D11Texture2D*        m_shared = nullptr;   // (Katanga's copy)
        int                     m_w = 0, m_h = 0;
    };
}
