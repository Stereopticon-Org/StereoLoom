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
    Zimo_SetModeFn      pSetMode = nullptr;
    Zimo_SetDepthFn     pSetDepth = nullptr;
    Zimo_PreferredDisplayFn pPreferred = nullptr;
    Mode                g_mode = Mode::EyeTracked;
    int                 g_features = -1;      // (what the display said it can do: -1 not known yet)
    bool                g_tab = false;        // (EyeTracked: the chip taking top and bottom)
    bool                g_want3D = false;
    int                 g_depth = 16;
    bool                g_blackoutOn = true;
    ULONGLONG           g_blackFrom = 0;      // (0: not black)
    bool                g_switching = false;  // (the display said a switch began, not yet that it ended)
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
        pSetMode   = (Zimo_SetModeFn)GetProcAddress(g_adapter, "Zimo_SetMode");
        pSetDepth  = (Zimo_SetDepthFn)GetProcAddress(g_adapter, "Zimo_SetDepth");
        pPreferred = (Zimo_PreferredDisplayFn)GetProcAddress(g_adapter, "Zimo_PreferredDisplay");
        if (!ver || ver() != ZIMO_ADAPTER_API_VERSION || !load || !pList || !pStart || !pSet3D || !pEyes || !pStatus || !pStop || !pDescribe || !pKeyKind
            || !pSetMode || !pSetDepth || !pPreferred)
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

    // The kind of device a mode drives.
    bool Suits(const ZimoDisplay& d, Mode m)
    {
        switch (m)
        {
        case Mode::Shutter:   return d.shutter != 0;
        case Mode::Polarised: return d.polarized != 0;
        case Mode::AR:        return d.ar != 0;
        default:              return d.zondision != 0;   // (an SR display listed by the SDK is never one)
        }
    }
    // The device for the mode: the one picked in Zondision's control panel if it suits,
    // else the first that does. False: none connected.
    bool Choose(Mode m, ZimoDisplay& out)
    {
        ZimoDisplay list[8]{};
        const int n = (std::min)(pList(list, 8), 8);
        const int preferred = pPreferred();
        int pick = -1;
        for (int i = 0; i < n; ++i)
            if (Suits(list[i], m) && (pick < 0 || list[i].id == preferred)) pick = i;
        if (pick < 0) return false;
        out = list[pick];
        return true;
    }
    int SdkMode()
    {
        switch (g_mode)
        {
        case Mode::MultiviewTB:    return ZIMO_MULTIVIEW_TB;
        case Mode::MultiviewDepth: return ZIMO_RGBDEPTH;
        case Mode::Shutter:        return ZIMO_SHUTTER;
        case Mode::Polarised:      return ZIMO_POLARIZED;
        case Mode::AR:             return ZIMO_AR;
        default:                   return g_tab ? ZIMO_TB : ZIMO_SBS;
        }
    }
    // The modes where the display's chip switches (and the screen flickers).
    bool ChipMode() { return g_mode == Mode::EyeTracked || g_mode == Mode::MultiviewTB || g_mode == Mode::MultiviewDepth; }
}

const char* ModeName(Mode m)
{
    switch (m)
    {
    case Mode::EyeTracked:     return "Glasses-free (eye tracked)";
    case Mode::MultiviewTB:    return "Multiview";
    case Mode::MultiviewDepth: return "Multiview (picture + depth)";
    case Mode::Shutter:        return "Shutter glasses";
    case Mode::Polarised:      return "Polarised glasses";
    case Mode::AR:             return "AR glasses";
    default:                   return "?";
    }
}

int Layout()
{
    switch (g_mode)
    {
    case Mode::MultiviewTB:    return 1;   // (OutputEncoder::ZLayout::TopBottom)
    case Mode::MultiviewDepth: return 4;   // (ColourDepth)
    case Mode::Shutter:        return 3;   // (Alternate)
    case Mode::Polarised:      return 2;   // (Rows)
    case Mode::AR:             return 0;   // (SideBySide: the glasses' double-wide mode)
    default:                   return g_tab ? 1 : 0;
    }
}

bool Available() { return Load(); }

unsigned AvailableModes()
{
    if (!Load()) return 0;
    ZimoDisplay list[8]{};
    const int n = (std::min)(pList(list, 8), 8);
    unsigned bits = 0;
    for (int i = 0; i < n; ++i)
        for (int m = 0; m < (int)Mode::Count; ++m)
            if (Suits(list[i], (Mode)m)) bits |= 1u << m;
    // (The multiview ones only where the display has said it has multiview firmware.)
    if (g_features < 2) bits &= ~((1u << (int)Mode::MultiviewTB) | (1u << (int)Mode::MultiviewDepth));
    return bits;
}

void SetMode(Mode m) { if ((int)m >= 0 && m < Mode::Count) g_mode = m; }
Mode GetMode() { return g_mode; }

bool FindDisplay(RECT& out, std::string* name)
{
    if (!Load()) return false;
    ZimoDisplay d{};
    if (!Choose(g_mode, d)) return false;
    const std::vector<Mon> mons = Monitors();
    // (By name first; failing that, the one monitor showing the device's native size.)
    for (int pass = 0; pass < 2; ++pass)
    {
        const Mon* hit = nullptr; int hits = 0;
        for (const Mon& m : mons)
        {
            const bool match = pass == 0 ? SameName(m.name, d.name)
                                         : (m.rc.right - m.rc.left == d.width && m.rc.bottom - m.rc.top == d.height);
            if (match) { hit = &m; ++hits; }
        }
        if (hit && (pass == 0 || hits == 1))
        {
            out = hit->rc;
            if (name) *name = d.name;
            return true;
        }
    }
    return false;
}

bool Start(std::string& msg)
{
    if (!Load()) { msg = "ZimoAdapter.dll or Zondision's SDK is missing"; return false; }
    if (g_started) return true;
    ZimoDisplay d{};
    if (!Choose(g_mode, d)) { msg = std::string("no Zondision device for ") + ModeName(g_mode) + " found"; return false; }
    g_tab = false;
    char ver[64] = {};
    int features = -1;
    const int r = pStart(d.id, SdkMode(), &features, ver, sizeof(ver));
    if (r != 0)
    {
        char b[240]; _snprintf_s(b, _TRUNCATE, "session not started (%s, %s): %s (%d)", d.name, ModeName(g_mode), pDescribe(r), r);
        msg = b;
        return false;
    }
    g_started = true;
    if (d.zondision) g_features = features;
    g_want3D = false; g_blackFrom = 0; g_switching = false;
    if (g_mode == Mode::MultiviewTB || g_mode == Mode::MultiviewDepth) pSetDepth(g_depth);
    char b[240];
    _snprintf_s(b, _TRUNCATE, "session started on %s (display %d), %s; the display %s (SDK %s)", d.name, d.id, ModeName(g_mode),
                features >= 2 ? "has multiview firmware" : features == 1 ? "weaves with its own chip" : "weaves on the GPU only", ver);
    msg = b;
    return true;
}

bool Set3D(bool on)
{
    if (!g_started) return false;
    const int r = pSet3D(on ? 1 : 0);
    if (r != 0) Log("Zondision: %s not taken: %s (%d)", on ? "3D" : "2D", pDescribe(r), r);
    if (r == 0 && on != g_want3D && ChipMode()) g_blackFrom = GetTickCount64();   // (the switch is coming: black)
    g_want3D = on && r == 0;
    return r == 0;
}

void Stop()
{
    if (!g_started) return;
    pStop();
    g_started = false;
    g_want3D = false; g_tab = false; g_blackFrom = 0; g_switching = false;
}

bool IsStarted() { return g_started; }

bool SetTopBottom(bool tab)
{
    tab = tab && g_started && g_mode == Mode::EyeTracked && g_features >= 2;
    if (tab == g_tab) return g_tab;
    const bool was = g_tab;
    g_tab = tab;
    const int r = pSetMode(SdkMode());
    if (r != 0)
    {
        Log("Zondision: %s not taken: %s (%d)", tab ? "top and bottom" : "side by side", pDescribe(r), r);
        g_tab = was;
        pSetMode(SdkMode());
        return g_tab;
    }
    Log("Zondision: the display takes %s", g_tab ? "top and bottom" : "side by side");
    if (g_want3D) g_blackFrom = GetTickCount64();
    return g_tab;
}

bool TopBottom() { return g_tab; }

void SetDepth(int depth)
{
    depth = depth < 0 ? 0 : depth > 62 ? 62 : depth;
    if (depth == g_depth) return;
    g_depth = depth;
    if (g_started && (g_mode == Mode::MultiviewTB || g_mode == Mode::MultiviewDepth)) pSetDepth(depth);
}

void SetBlackoutEnabled(bool on) { g_blackoutOn = on; if (!on) { g_blackFrom = 0; g_switching = false; } }

// Black from asking for the switch until the display says it is done -- at most 2 s
// (Zondision's ZLFDDisableBlackScreenTimeout); and if it never says a switch began,
// half a second (their sample waits that long for it).
bool Blackout()
{
    if (!g_blackoutOn || !g_started || !g_blackFrom) return false;
    const ULONGLONG t = GetTickCount64() - g_blackFrom;
    if (t > 2000 || (!g_switching && t > 500)) { g_blackFrom = 0; g_switching = false; return false; }
    return true;
}

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
    {
        Log("Zondision: %s %d", act >= 0 && act < 8 ? kAct[act] : "status", value);
        if (act == 1)   // (a switch begins: black until it ends)
        {
            if (ChipMode()) { g_switching = true; g_blackFrom = GetTickCount64(); }
        }
        else if (act == 3) { g_switching = false; g_blackFrom = 0; }
        // (Zondision's service back after a restart: 3D asked for again -- the adapter
        // starts the session afresh when the service has forgotten it. A monitor
        // plugged back in is asked again by the adapter itself.)
        else if (act == 5 && value == 1 && g_want3D)
        {
            const int r = pSet3D(1);
            Log("Zondision: 3D asked for again after the service came back: %s", r == 0 ? "taken" : pDescribe(r));
        }
    }
}
}
