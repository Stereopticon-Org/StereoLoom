// Vision3D.h -- the NVIDIA 3D Vision output: shutter glasses on a 3D Vision panel,
// through NV3D-Lib (github.com/oneup03/NV3D-Lib, LGPL 2.1), as wiz3D does it. The
// library opens its own exclusive-fullscreen Direct3D 9 window on the display and
// hands the driver the two eyes side by side behind NVIDIA's stereo signature; the
// driver shows them in turn to the glasses. Needs an NVIDIA driver with 3D Vision:
// R425 or older, or a newer one with 3D Vision brought back (Vision Restoration,
// github.com/oscardelatorre/VisionRestoration), and "Set up stereoscopic 3D" on in
// the NVIDIA Control Panel. Built only where NV3D-Lib is in lib/NV3D-Lib.
#pragma once

#include "Common.h"
#include <d3d11.h>
#include <string>

namespace srw
{
    class Vision3D
    {
    public:
        ~Vision3D() { Stop(); }
        // This build has it (NV3D-Lib was there to link).
        static bool Built();
        // On the display `monitor`, whose size the side-by-side picture is made for.
        bool Start(ID3D11Device* device, HMONITOR monitor, std::string& msg);
        void Stop();
        bool IsUp() const { return m_iface != nullptr; }
        // The side-by-side picture to draw into (2W x H, BGRA, shared), then Present.
        ID3D11RenderTargetView* Target() const { return m_rtv; }
        int Width() const { return m_w; }     // (one eye's)
        int Height() const { return m_h; }
        void Present();

    private:
        void* m_iface = nullptr;   // NV3D::InterfaceDX11 (kept out of this header)
        ID3D11Texture2D*        m_tex = nullptr;
        ID3D11RenderTargetView* m_rtv = nullptr;
        int                     m_w = 0, m_h = 0;
        ULONGLONG               m_statsAt = 0;
    };
}
