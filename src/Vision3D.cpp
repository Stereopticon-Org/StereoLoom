// Vision3D.cpp -- see Vision3D.h.
#include "Vision3D.h"

#ifdef SRLOOM_NV3D
#include "NV3D.hpp"
#endif

namespace srw
{
#ifdef SRLOOM_NV3D
namespace
{
    void __cdecl LogSink(NV3D::LogLevel level, const wchar_t* message, void*)
    {
        if (level == NV3D::LogLevel::Debug || !message) return;
        Log("3D Vision (NV3D-Lib): %ls", message);
    }
    NV3D::InterfaceDX11* Iface(void* p) { return static_cast<NV3D::InterfaceDX11*>(p); }
}

bool Vision3D::Built() { return true; }

bool Vision3D::Start(ID3D11Device* device, HMONITOR monitor, std::string& msg)
{
    Stop();
    if (!device) { msg = "no device"; return false; }
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(monitor, &mi)) { msg = "no display"; return false; }
    m_w = mi.rcMonitor.right - mi.rcMonitor.left;
    m_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    // The side-by-side picture: BGRA and shared, the library's fast path.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)(m_w * 2); td.Height = (UINT)m_h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &m_tex)) || FAILED(device->CreateRenderTargetView(m_tex, nullptr, &m_rtv)))
    {
        Stop();
        msg = "the side-by-side texture could not be made";
        return false;
    }
    NV3D::SetLogSink(LogSink, nullptr);
    NV3D::InitParams p{};
    p.target_monitor = monitor;
    p.eye_swap = false;              // (Swap Eyes is done in Stereo Loom's own drawing)
    p.on_top = true;
    p.enable_lightboost = true;
    p.enable_suppressor = true;
    p.host_hwnd = nullptr;           // (the library's own popup: Stereo Loom's window is a DXGI one)
    // (Kept on top while another program has the focus -- the one being shown in
    // 3D, usually -- rather than minimised as soon as Stereo Loom loses it.)
    DWORD pid = 0;
    if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
    p.tracked_game_pid = pid ? pid : GetCurrentProcessId();
    NV3D::InterfaceDX11* iface = nullptr;
    const HRESULT hr = NV3D::CreateInterfaceDX11(device, &p, &iface);
    if (FAILED(hr) || !iface)
    {
        Stop();
        char b[200];
        _snprintf_s(b, _TRUNCATE, "not started (0x%08X): is 3D Vision installed (an older driver, or Vision Restoration) and "
                    "\"Set up stereoscopic 3D\" on in the NVIDIA Control Panel?", (unsigned)hr);
        msg = b;
        return false;
    }
    m_iface = iface;
    iface->SetInputTexture(m_tex);
    char b[160];
    _snprintf_s(b, _TRUNCATE, "started on a %dx%d display (side by side %dx%d)", m_w, m_h, m_w * 2, m_h);
    msg = b;
    m_statsAt = GetTickCount64();
    return true;
}

void Vision3D::Stop()
{
    if (m_iface) { Iface(m_iface)->Delete(); m_iface = nullptr; }
    SAFE_RELEASE(m_rtv);
    SAFE_RELEASE(m_tex);
}

void Vision3D::Present()
{
    if (!m_iface) return;
    Iface(m_iface)->Present();
    // (Now and then, to the log: whether the glasses get every pair.)
    if (GetTickCount64() - m_statsAt > 10000)
    {
        m_statsAt = GetTickCount64();
        NV3D::PresentStats s{};
        Iface(m_iface)->GetPresentStats(&s);
        Log("3D Vision: %u pairs shown, %u dropped (the glasses' side busy), %.1f ms each (most %.1f)",
            s.presents_done, s.submits_dropped, s.present_ms_avg, s.present_ms_max);
    }
}
#else
bool Vision3D::Built() { return false; }
bool Vision3D::Start(ID3D11Device*, HMONITOR, std::string& msg) { msg = "this build has no 3D Vision (NV3D-Lib was not in lib/NV3D-Lib)"; return false; }
void Vision3D::Stop() {}
void Vision3D::Present() {}
#endif
}
