// Zondision.cpp -- see Zondision.h.
#include "Zondision.h"
#include "Common.h"
#include "resource.h"
#include "../adapters/zimo/ZimoAdapter.h"

#include <shlobj.h>
#include <vector>
#include <cmath>

namespace srw::Zondision
{
namespace
{
    HMODULE g_adapter = nullptr;
    int     g_state = -1;   // (-1 not looked yet, 0 not there, 1 there)
    bool    g_started = false;
    Zimo_ListDisplaysFn pList = nullptr;
    Zimo_StartFn        pStart = nullptr;
    Zimo_Set3DFn        pSet3D = nullptr;
    Zimo_GetEyesFn      pEyes = nullptr;
    Zimo_TakeStatusFn   pStatus = nullptr;
    Zimo_StopFn         pStop = nullptr;
    Zimo_DescribeFn     pDescribe = nullptr;
    Zimo_KeyKindFn      pKeyKind = nullptr;
    unsigned            g_eyeSerial = 0;
    ULONGLONG           g_eyeLogAt = 0;

    // The exe carries ZimoAdapter.dll and the SDK's DLLs (SRLOOM_ZIMO_EMBED): unpacked
    // into %LOCALAPPDATA%\SRLoom\Zondision\<hash of the three>\ the first time they
    // are wanted, so a newer build never meets an older copy (or one still in use).
    // Returns the adapter's path there, or "" (nothing carried / couldn't write).
    std::string UnpackEmbedded()
    {
#ifdef SRLOOM_ZIMO_EMBED
        struct Part { int id; const char* name; const void* data = nullptr; DWORD size = 0; };
        Part parts[] = { { IDR_ZIMO_ADAPTER, "ZimoAdapter.dll" }, { IDR_ZIMO_SDK, "3DS_SDK.dll" }, { IDR_ZIMO_JSONCPP, "jsoncpp.dll" } };
        unsigned long long hash = 1469598103934665603ull;   // (FNV-1a)
        for (Part& p : parts)
        {
            HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(p.id), MAKEINTRESOURCEW(10));   // (RT_RCDATA)
            HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
            p.data = g ? LockResource(g) : nullptr;
            p.size = r ? SizeofResource(nullptr, r) : 0;
            if (!p.data || !p.size) return {};
            const unsigned char* b = (const unsigned char*)p.data;
            for (DWORD i = 0; i < p.size; ++i) { hash ^= b[i]; hash *= 1099511628211ull; }
        }
        PWSTR base = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) || !base) return {};
        wchar_t sub[32]; swprintf_s(sub, L"%016llx", hash);
        std::wstring dir = std::wstring(base) + L"\\SRLoom\\Zondision\\" + sub;
        CoTaskMemFree(base);
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        for (const Part& p : parts)
        {
            wchar_t wname[64]; MultiByteToWideChar(CP_ACP, 0, p.name, -1, wname, 64);
            const std::wstring file = dir + L"\\" + wname;
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeHigh == 0 && fa.nFileSizeLow == p.size)
                continue;   // (already there: the folder's name is the content's hash)
            const std::wstring tmp = file + L".tmp";
            HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) { Log("Zondision: could not unpack %s", p.name); return {}; }
            DWORD wrote = 0;
            const BOOL ok = WriteFile(h, p.data, p.size, &wrote, nullptr) && wrote == p.size;
            CloseHandle(h);
            if (!ok || !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                DeleteFileW(tmp.c_str());
                Log("Zondision: could not unpack %s", p.name);
                return {};
            }
        }
        const std::wstring adapter = dir + L"\\ZimoAdapter.dll";
        const int n = WideCharToMultiByte(CP_ACP, 0, adapter.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string out(n > 0 ? n - 1 : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_ACP, 0, adapter.c_str(), -1, out.data(), n, nullptr, nullptr);
        return out;
#else
        return {};
#endif
    }

    bool Load()
    {
        if (g_state >= 0) return g_state == 1;
        g_state = 0;
        // (a ZimoAdapter.dll beside the exe wins -- a development build -- else the
        // copy the exe carries; its SDK DLLs are found beside it either way)
        g_adapter = LoadLibraryExA(ExePath("ZimoAdapter.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!g_adapter)
        {
            const std::string carried = UnpackEmbedded();
            if (!carried.empty()) g_adapter = LoadLibraryExA(carried.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        if (!g_adapter) { Log("Zondision: ZimoAdapter.dll not available (Zondision output unavailable)"); return false; }
        auto ver   = (Zimo_ApiVersionFn)GetProcAddress(g_adapter, "Zimo_ApiVersion");
        auto load  = (Zimo_LoadFn)GetProcAddress(g_adapter, "Zimo_Load");
        pList      = (Zimo_ListDisplaysFn)GetProcAddress(g_adapter, "Zimo_ListDisplays");
        pStart     = (Zimo_StartFn)GetProcAddress(g_adapter, "Zimo_Start");
        pSet3D     = (Zimo_Set3DFn)GetProcAddress(g_adapter, "Zimo_Set3D");
        pEyes      = (Zimo_GetEyesFn)GetProcAddress(g_adapter, "Zimo_GetEyes");
        pStatus    = (Zimo_TakeStatusFn)GetProcAddress(g_adapter, "Zimo_TakeStatus");
        pStop      = (Zimo_StopFn)GetProcAddress(g_adapter, "Zimo_Stop");
        pDescribe  = (Zimo_DescribeFn)GetProcAddress(g_adapter, "Zimo_Describe");
        pKeyKind   = (Zimo_KeyKindFn)GetProcAddress(g_adapter, "Zimo_KeyKind");
        if (!ver || ver() != ZIMO_ADAPTER_API_VERSION || !load || !pList || !pStart || !pSet3D || !pEyes || !pStatus || !pStop || !pDescribe || !pKeyKind)
        {
            Log("Zondision: ZimoAdapter.dll is not the version this Stereo Loom expects");
            FreeLibrary(g_adapter); g_adapter = nullptr;
            return false;
        }
        const int r = load();
        if (r != 0)
        {
            Log("Zondision: Zondision's SDK could not be loaded: %s (%d)", pDescribe(r), r);
            return false;
        }
        Log("Zondision: adapter and SDK loaded (%s app key)", pKeyKind() ? "Stereo Loom's own" : "Zondision's evaluation");
        g_state = 1;
        return true;
    }

    // Each monitor: its rect, current size and EDID name (QueryDisplayConfig).
    struct Mon { RECT rc; std::wstring device; std::string name; };
    BOOL CALLBACK MonProc(HMONITOR m, HDC, LPRECT, LPARAM p)
    {
        MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(m, &mi)) reinterpret_cast<std::vector<Mon>*>(p)->push_back({ mi.rcMonitor, mi.szDevice, {} });
        return TRUE;
    }
    std::vector<Mon> Monitors()
    {
        std::vector<Mon> mons;
        EnumDisplayMonitors(nullptr, nullptr, MonProc, reinterpret_cast<LPARAM>(&mons));
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return mons;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return mons;
        for (UINT32 i = 0; i < np; ++i)
        {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
            src.header = { DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(src), paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id };
            DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
            tgt.header = { DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(tgt), paths[i].targetInfo.adapterId, paths[i].targetInfo.id };
            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;
            for (Mon& m : mons)
                if (m.device == src.viewGdiDeviceName)
                {
                    char n[64] = {};
                    WideCharToMultiByte(CP_UTF8, 0, tgt.monitorFriendlyDeviceName, -1, n, sizeof(n) - 1, nullptr, nullptr);
                    m.name = n;
                }
        }
        return mons;
    }
    bool SameName(const std::string& a, const char* b)
    {
        if (a.empty() || !b || !*b) return false;
        return _stricmp(a.c_str(), b) == 0 || strstr(a.c_str(), b) || strstr(b, a.c_str());
    }
}

bool Available() { return Load(); }

bool FindDisplay(RECT& out, std::string* name)
{
    if (!Load()) return false;
    ZimoDisplay list[8]{};
    const int n = (std::min)(pList(list, 8), 8);
    const std::vector<Mon> mons = Monitors();
    // (By name first; failing that, the one monitor showing the display's native size.)
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < n; ++i)
        {
            if (!list[i].zondision) continue;   // (an SR display listed by the SDK is not one for this output)
            const Mon* hit = nullptr; int hits = 0;
            for (const Mon& m : mons)
            {
                const bool match = pass == 0 ? SameName(m.name, list[i].name)
                                             : (m.rc.right - m.rc.left == list[i].width && m.rc.bottom - m.rc.top == list[i].height);
                if (match) { hit = &m; ++hits; }
            }
            if (hit && (pass == 0 || hits == 1))
            {
                out = hit->rc;
                if (name) *name = list[i].name;
                return true;
            }
        }
    return false;
}

bool Start(std::string& msg)
{
    if (!Load()) { msg = "ZimoAdapter.dll or Zondision's SDK is missing"; return false; }
    if (g_started) return true;
    // (Only a Zondision display: the SDK also lists Leia SR displays, and its first
    // 3D display could be one of those.)
    int id = 0;
    {
        ZimoDisplay list[8]{};
        const int n = (std::min)(pList(list, 8), 8);
        for (int i = 0; i < n && !id; ++i) if (list[i].zondision) id = list[i].id;
    }
    if (!id) { msg = "no Zondision display found"; return false; }
    char ver[64] = {};
    const int r = pStart(id, ver, sizeof(ver));
    if (r != 0)
    {
        char b[200]; _snprintf_s(b, _TRUNCATE, "session not started: %s (%d)", pDescribe(r), r);
        msg = b;
        return false;
    }
    g_started = true;
    msg = std::string("session started (SDK ") + ver + ")";
    return true;
}

bool Set3D(bool on)
{
    if (!g_started) return false;
    const int r = pSet3D(on ? 1 : 0);
    if (r != 0) Log("Zondision: %s not taken: %s (%d)", on ? "3D" : "2D", pDescribe(r), r);
    return r == 0;
}

void Stop()
{
    if (!g_started) return;
    pStop();
    g_started = false;
}

bool IsStarted() { return g_started; }

bool GetHeadPose(double pos[3], double orient[3])
{
    if (!g_started) return false;
    ZimoEyes e{};
    if (!pEyes(&e) || e.faces <= 0) return false;
    double c[3];
    for (int k = 0; k < 3; ++k) c[k] = 0.5 * ((double)e.left[k] + (double)e.right[k]);
    // (The SDK does not say its unit. A viewer sits roughly 0.3 to 1.5 m away: a
    // distance under 5 is metres, under 200 centimetres, else millimetres.)
    const double z = std::fabs(c[2]);
    const double toMm = z < 5.0 ? 1000.0 : z < 200.0 ? 10.0 : 1.0;
    for (int k = 0; k < 3; ++k) pos[k] = c[k] * toMm;
    const double d2r = 3.14159265358979 / 180.0;
    for (int k = 0; k < 3; ++k) orient[k] = (double)e.headRotation[k] * d2r;
    // (Now and then, to the log as given: to check the unit and axes.)
    if (e.serial != g_eyeSerial && GetTickCount64() - g_eyeLogAt > 10000)
    {
        g_eyeLogAt = GetTickCount64();
        Log("Zondision: eyes L (%.3f, %.3f, %.3f) R (%.3f, %.3f, %.3f) as given -> head (%.0f, %.0f, %.0f) mm",
            e.left[0], e.left[1], e.left[2], e.right[0], e.right[1], e.right[2], pos[0], pos[1], pos[2]);
    }
    g_eyeSerial = e.serial;
    return true;
}

void Poll()
{
    if (!g_started) return;
    static const char* const kAct[] = { "3D mode", "before mode switch", "display chip reset", "after mode switch",
                                        "USB", "3DS service", "film", "monitor" };
    int act = 0, value = 0;
    while (pStatus(&act, &value))
        Log("Zondision: %s %d", act >= 0 && act < 8 ? kAct[act] : "status", value);
}
}
