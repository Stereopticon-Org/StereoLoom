// KatangaOutput.cpp -- see KatangaOutput.h.
#include "KatangaOutput.h"
#include <dxgi.h>

namespace srw
{
namespace
{
    const wchar_t kMutexName[] = L"KatangaSetupMutex";
    const wchar_t kFileName[]  = L"Local\\KatangaMappedFile";
    struct Held
    {
        explicit Held(HANDLE m) : m(m), ok(m && WaitForSingleObject(m, 1000) == WAIT_OBJECT_0) {}
        ~Held() { if (ok) ReleaseMutex(m); }
        HANDLE m; bool ok;
    };
}

bool KatangaOutput::KatangaRunning()
{
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, kMutexName);
    if (!h) return false;
    CloseHandle(h);
    return true;
}

bool KatangaOutput::Start()
{
    if (IsUp()) return true;
    m_mutex = OpenMutexW(SYNCHRONIZE, FALSE, kMutexName);
    if (!m_mutex) return false;
    m_file = OpenFileMappingW(FILE_MAP_WRITE, FALSE, kFileName);
    m_view = m_file ? MapViewOfFile(m_file, FILE_MAP_WRITE, 0, 0, sizeof(UINT)) : nullptr;
    if (!m_view) { Stop(); return false; }
    return true;
}

void KatangaOutput::ReleaseTextures()
{
    // (No producer, said first: Katanga stops reading before the texture goes.)
    if (m_mutex && m_view) { Held h(m_mutex); if (h.ok) *static_cast<UINT*>(m_view) = 0; }
    SAFE_RELEASE(m_rtv); SAFE_RELEASE(m_draw); SAFE_RELEASE(m_shared);
    m_w = m_h = 0;
}

void KatangaOutput::Stop()
{
    ReleaseTextures();
    if (m_view) { UnmapViewOfFile(m_view); m_view = nullptr; }
    if (m_file) { CloseHandle(m_file); m_file = nullptr; }
    if (m_mutex) { CloseHandle(m_mutex); m_mutex = nullptr; }
}

ID3D11RenderTargetView* KatangaOutput::Target(ID3D11Device* device, int eyeW, int eyeH)
{
    if (!IsUp() || !device || eyeW <= 0 || eyeH <= 0) return nullptr;
    if (m_rtv && m_w == eyeW * 2 && m_h == eyeH) return m_rtv;
    ReleaseTextures();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)(eyeW * 2); d.Height = (UINT)eyeH; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &m_draw)) || FAILED(device->CreateRenderTargetView(m_draw, nullptr, &m_rtv)))
    { ReleaseTextures(); return nullptr; }
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;   // (the legacy shared handle Katanga opens)
    HANDLE shared = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&d, nullptr, &m_shared)))
    {
        IDXGIResource* r = nullptr;
        if (SUCCEEDED(m_shared->QueryInterface(__uuidof(IDXGIResource), (void**)&r))) { r->GetSharedHandle(&shared); r->Release(); }
    }
    if (!shared) { ReleaseTextures(); return nullptr; }
    {
        Held h(m_mutex);
        if (!h.ok) { ReleaseTextures(); return nullptr; }
        *static_cast<UINT*>(m_view) = PtrToUint(shared);
    }
    m_w = eyeW * 2; m_h = eyeH;
    Log("Katanga output: side by side %dx%d handed to Katanga", m_w, m_h);
    return m_rtv;
}

void KatangaOutput::Publish(ID3D11DeviceContext* ctx)
{
    if (!ctx || !m_shared || !m_draw) return;
    Held h(m_mutex);
    if (h.ok) ctx->CopyResource(m_shared, m_draw);
}
}
