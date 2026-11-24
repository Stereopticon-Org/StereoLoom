// ZimoAdapter.cpp -- ZimoAdapter.dll: Stereo Loom's link to a Zondision 3D display
// through Zondision's 3DS SDK (3DS_SDK.dll). See ZimoAdapter.h.
//
// The app key: Zondision's SDK identifies each application by one, and asks that an
// open-source project keeps its key inside a binary, not in a text file. So:
//   - Stereo Loom's own key lives in ZimoKey.private.h, beside this file, which is
//     never committed (.gitignore). A release build of this DLL has it compiled in.
//   - Without that file, the build uses Zondision's evaluation key -- published by
//     Zondision in its SDK sample, free to release, and limited in time. It is kept
//     here already scrambled , so it is not readable in the source either.
// Either way the key is stored XOR-scrambled (worked out at compile time), so it is
// not lying in the DLL as plain text, and the clear copy is wiped once the SDK has
// it. (That keeps it out of casual view; it is not, and cannot be, secret from a
// determined reader of the DLL -- which Zondision know and accept.)
#include "ZimoAdapter.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

#include "3DS_SDK.h"

#if __has_include("ZimoKey.private.h")
#include "ZimoKey.private.h"            // #define ZIMO_APP_KEY "..." (Stereo Loom's own)
#define ZIMO_KEY_KIND 1
#else
#define ZIMO_KEY_KIND 0                 // (Zondision's evaluation key: below, scrambled)
#endif

namespace
{
    // ---- The key, scrambled at compile time --------------------------------
    constexpr unsigned char KeyByte(size_t i) { return (unsigned char)(0xA7u ^ (i * 0x3Du) ^ (i >> 3) * 0x5Bu); }
    template <size_t N>
    struct Scrambled
    {
        char d[N];
        constexpr Scrambled(const char (&s)[N]) : d{}
        {
            for (size_t i = 0; i < N; ++i) d[i] = (char)((unsigned char)s[i] ^ KeyByte(i));
        }
        void Into(char* out, size_t outSize) const
        {
            for (size_t i = 0; i < N && i < outSize; ++i) out[i] = (char)((unsigned char)d[i] ^ KeyByte(i));
            if (outSize) out[(N < outSize ? N : outSize) - 1] = '\0';
        }
    };
#if ZIMO_KEY_KIND
    constexpr Scrambled<sizeof(ZIMO_APP_KEY)> kKey(ZIMO_APP_KEY);
#else
    // Zondision's evaluation key, from their SDK sample (expires 2027-01-01), as
    // Scrambled holds it: each byte (and the closing 0) XORed with KeyByte(i).
    struct ScrambledBytes
    {
        static constexpr unsigned char d[] = {
            0x95, 0xCA, 0x8A, 0x7A, 0x07, 0xF2, 0xBC, 0x6D, 0x47, 0x93, 0xF4, 0x3A, 0x62, 0x9D, 0xC5, 0x2E,
            0xB5, 0x44, 0x28, 0xFE, 0xAF, 0x59, 0x75, 0x3D, 0x49, 0x2B, 0xB4, 0x95, 0x50, 0x69, 0xD1, 0xA0,
            0x46, 0x6C, 0xE3, 0xF8, 0x00, 0x2A, 0xFD, 0xDF, 0x8E, 0x9D, 0x53, 0x6C, 0x2C, 0xE9, 0xF3, 0x36,
            0xC7, 0x1E, 0x5C, 0x9A, 0xD5, 0x17, 0x39, 0xA8, 0xAF, 0x7D, 0x3D, 0xB4, 0xF4, 0x60, 0x7F, 0xE0,
            0x06, 0x31, 0xF6, 0xEA, 0x7D, 0x3A, 0xE1, 0xA3, 0x89, 0x90, 0x57, 0x7F, 0xBE, 0xFC, 0x31, 0x72,
            0x5C, 0x06, 0xC2, 0xDA, 0x4F, 0x0A, 0x60, 0xAA, 0x84, 0x7B,
        };
        void Into(char* out, size_t outSize) const
        {
            constexpr size_t N = sizeof(d);
            for (size_t i = 0; i < N && i < outSize; ++i) out[i] = (char)(d[i] ^ KeyByte(i));
            if (outSize) out[(N < outSize ? N : outSize) - 1] = '\0';
        }
    };
    constexpr ScrambledBytes kKey{};
#endif

    // ---- The SDK, loaded at run time ----------------------------------------
    HMODULE g_dll = nullptr;
    Client_CreateSessionFunc       pCreateSession = nullptr;
    Client_Start3DSessionFunc      pStart3DSession = nullptr;
    Client_Stop3DSessionFunc       pStop3DSession = nullptr;
    Client_CloseSessionFunc        pCloseSession = nullptr;
    Client_Set3DModeFunc           pSet3DMode = nullptr;
    Client_SetAuto2DFunc           pSetAuto2D = nullptr;
    Client_SubscribeEyeDataFunc    pSubscribeEyeData = nullptr;
    Client_UnSubscribeEyeDataFunc  pUnSubscribeEyeData = nullptr;
    Client_SubscribeStatusChangeFunc   pSubscribeStatus = nullptr;
    Client_UnSubscribeStatusChangeFunc pUnSubscribeStatus = nullptr;
    Get3DDisplayListFunc           pGet3DDisplayList = nullptr;
    Client_SetK3DepthFunc          pSetK3Depth = nullptr;        // (optional)
    GetPreferredDisplayIDFunc      pGetPreferredDisplay = nullptr;   // (optional: newer SDKs)

    INT_PTR       g_session = 0;
    bool          g_started = false;
    SESSIONCONFIG g_cfg{};
    WindowRect    g_windowRect{};
    std::atomic<int> g_want3D{ 0 };

    std::mutex    g_mutex;   // (the SDK's callbacks come on its own threads)
    ZimoEyes      g_eyes{};
    bool          g_haveEyes = false;
    std::deque<std::pair<int, int>> g_status;

    void __cdecl OnEyes(EyeParams* p, INT_PTR)        // (SDK 5.0.4.5+: the session id too)
    {
        if (!p) return;
        std::lock_guard<std::mutex> lk(g_mutex);
        g_eyes.faces = p->FaceNum;
        g_eyes.left[0]  = p->LeftEyePosSmoothed.x;  g_eyes.left[1]  = p->LeftEyePosSmoothed.y;  g_eyes.left[2]  = p->LeftEyePosSmoothed.z;
        g_eyes.right[0] = p->RightEyePosSmoothed.x; g_eyes.right[1] = p->RightEyePosSmoothed.y; g_eyes.right[2] = p->RightEyePosSmoothed.z;
        g_eyes.headRotation[0] = p->HeadRotation.x; g_eyes.headRotation[1] = p->HeadRotation.y; g_eyes.headRotation[2] = p->HeadRotation.z;
        ++g_eyes.serial;
        g_haveEyes = true;
    }

    void __cdecl OnStatus(_3DS_STATUS* s, INT_PTR)
    {
        if (!s) return;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_status.size() < 64) g_status.emplace_back((int)s->act, s->value);
        }
        // (The display's chip was reset -- a resolution change -- or the monitor
        // was plugged back in: it may have forgotten the mode, so 3D is asked for
        // again. Zondision's sample.)
        const bool back = s->act == ACT_ASICRESETTING || (s->act == ACT_MONITORSTATUS && s->value == 1);
        if (back && g_want3D.load() && pSet3DMode && g_started)
            pSet3DMode(_3D, g_session);
    }

    // Beside this DLL first (as Stereo Loom ships it), then the usual search.
    HMODULE LoadSdk()
    {
        wchar_t path[MAX_PATH] = {};
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&LoadSdk, &self);
        if (GetModuleFileNameW(self, path, MAX_PATH))
        {
            if (wchar_t* slash = wcsrchr(path, L'\\'))
            {
                wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"3DS_SDK.dll");
                if (HMODULE h = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return h;
            }
        }
        return LoadLibraryW(L"3DS_SDK.dll");
    }

    template <class T> bool Find(T& fn, const char* name)
    {
        fn = reinterpret_cast<T>(GetProcAddress(g_dll, name));
        return fn != nullptr;
    }
}

extern "C"
{
__declspec(dllexport) int __cdecl Zimo_ApiVersion(void) { return ZIMO_ADAPTER_API_VERSION; }

__declspec(dllexport) int __cdecl Zimo_KeyKind(void) { return ZIMO_KEY_KIND; }

__declspec(dllexport) int __cdecl Zimo_Load(void)
{
    if (g_dll) return 0;
    g_dll = LoadSdk();
    if (!g_dll) return (int)FILE_NOT_FOUND;
    const bool ok = Find(pCreateSession, "Client_CreateSession") && Find(pStart3DSession, "Client_Start3DSession")
                 && Find(pStop3DSession, "Client_Stop3DSession") && Find(pCloseSession, "Client_CloseSession")
                 && Find(pSet3DMode, "Client_Set3DMode") && Find(pSetAuto2D, "Client_SetAuto2D")
                 && Find(pSubscribeEyeData, "Client_SubscribeEyeData") && Find(pUnSubscribeEyeData, "Client_UnSubscribeEyeData")
                 && Find(pSubscribeStatus, "Client_SubscribeStatusChange") && Find(pUnSubscribeStatus, "Client_UnSubscribeStatusChange")
                 && Find(pGet3DDisplayList, "Get3DDisplayList");
    if (ok)
    {
        Find(pSetK3Depth, "Client_SetK3Depth");
        Find(pGetPreferredDisplay, "GetPreferredDisplayID");
    }
    if (!ok)
    {
        FreeLibrary(g_dll);
        g_dll = nullptr;
        return (int)INTERNAL_DATA_ERROR;
    }
    return 0;
}

__declspec(dllexport) int __cdecl Zimo_ListDisplays(ZimoDisplay* out, int max)
{
    if (Zimo_Load() != 0) return 0;
    int n = 0;
    if (pGet3DDisplayList(nullptr, &n) != SUCCESS || n <= 0) return 0;
    DISPLAYINF* list = new DISPLAYINF[n]();
    int cap = n;
    if (pGet3DDisplayList(list, &cap) != SUCCESS) cap = 0;
    const int count = cap < n ? cap : n;
    for (int i = 0; i < count && out && i < max; ++i)
    {
        ZimoDisplay& d = out[i];
        memset(&d, 0, sizeof(d));
        d.id = list[i].displayId;
        memcpy(d.name, list[i].friendlyName, sizeof(d.name) - 1);
        d.width = list[i].nativeWidth; d.height = list[i].nativeHeight;
        d.mmWidth = list[i].physicalWidth; d.mmHeight = list[i].physicalHeight;
        d.zondision = list[i].ZLFDRegistered ? 1 : 0;
        d.sr = list[i].SRRegistered ? 1 : 0;
        memcpy(d.serial, list[i].SerialNumber, sizeof(list[i].SerialNumber) < sizeof(d.serial) ? sizeof(list[i].SerialNumber) : sizeof(d.serial) - 1);
        d.shutter = list[i].ShutterRegistered ? 1 : 0;
        d.polarized = list[i].PolarizedRegistered ? 1 : 0;
        d.ar = list[i].ARRegistered ? 1 : 0;
    }
    delete[] list;
    return count;
}

__declspec(dllexport) void __cdecl Zimo_Stop(void)
{
    if (!g_started) return;
    g_want3D = 0;
    pSet3DMode(_2D, g_session);   // (the display left in 2D, as Zondision's sample does)
    pUnSubscribeEyeData(OnEyes, g_session);
    pUnSubscribeStatus(OnStatus, g_session);
    pStop3DSession(g_session);
    pCloseSession(g_session);
    g_session = 0;
    g_started = false;
    std::lock_guard<std::mutex> lk(g_mutex);
    g_haveEyes = false;
    g_status.clear();
}

__declspec(dllexport) int __cdecl Zimo_Start(int id, int mode, int* features, char* sdkVersion, int sdkVersionSize)
{
    if (g_started) Zimo_Stop();
    if (const int r = Zimo_Load()) return r;
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_windowRect = WindowRect{};
    kKey.Into(g_cfg.appKey, sizeof(g_cfg.appKey));
    g_cfg.clientType      = Game3DTransposer;   // (Stereo Loom turns games and video into 3D for the display)
    g_cfg.cameraBehaviour = CAMERA_FOLLOW_3D;   // (the camera off whenever 3D is)
    g_cfg.windowRectClient = &g_windowRect;
    g_cfg.weavingMode     = mode >= HARDWARE_WEAVING_SBS && mode <= SHUTTER ? (WeavingMode)mode : HARDWARE_WEAVING_SBS;
    g_cfg.ipcType         = Socket;
    if (id > 0)
    {
        DISPLAYINF list[16]{};
        int cap = 16;
        if (pGet3DDisplayList(list, &cap) == SUCCESS)
            for (int i = 0; i < cap && i < 16; ++i)
                if (list[i].displayId == id) g_cfg.displaySelected = list[i];
    }
    g_session = pCreateSession();
    const RET r = pStart3DSession(&g_cfg, g_session);
    SecureZeroMemory(g_cfg.appKey, sizeof(g_cfg.appKey));   // (the SDK has it now)
    if (r != SUCCESS)
    {
        pCloseSession(g_session);
        g_session = 0;
        return (int)r;
    }
    g_started = true;
    if (sdkVersion && sdkVersionSize > 0) strncpy_s(sdkVersion, sdkVersionSize, g_cfg.SDKVersion, _TRUNCATE);
    if (features) *features = (int)g_cfg.platformFeatures;
    // (Never 2D on its own when nobody is seen: Stereo Loom keeps the 3D on.)
    pSetAuto2D(Off, g_session);
    pSubscribeStatus(OnStatus, g_session);
    pSubscribeEyeData(OnEyes, g_session);
    return 0;
}

__declspec(dllexport) int __cdecl Zimo_Set3D(int on)
{
    if (!g_started) return (int)SESSION_NOT_FOUND;
    g_want3D = on ? 1 : 0;
    RET r = pSet3DMode(on ? _3D : _2D, g_session);
    if (r == GENERAL_ERR)
    {
        // (Zondision's sample: the 3DS service may have restarted -- the session
        // started again, with its subscriptions, and asked once more.)
        kKey.Into(g_cfg.appKey, sizeof(g_cfg.appKey));
        const RET s = pStart3DSession(&g_cfg, g_session);
        SecureZeroMemory(g_cfg.appKey, sizeof(g_cfg.appKey));
        if (s == SUCCESS)
        {
            pSetAuto2D(Off, g_session);
            pSubscribeStatus(OnStatus, g_session);
            pSubscribeEyeData(OnEyes, g_session);
            r = pSet3DMode(on ? _3D : _2D, g_session);
        }
    }
    return (int)r;
}

// (The SDK keeps the session's config and reads the mode from it when 3D is set:
// Zondision's sample changes modes this way, without a new session.)
__declspec(dllexport) int __cdecl Zimo_SetMode(int mode)
{
    if (!g_started) return (int)SESSION_NOT_FOUND;
    if (mode < HARDWARE_WEAVING_SBS || mode > SHUTTER) return (int)INVALID_PARAM;
    if (g_cfg.weavingMode == (WeavingMode)mode) return 0;
    g_cfg.weavingMode = (WeavingMode)mode;
    return g_want3D.load() ? (int)pSet3DMode(_3D, g_session) : 0;
}

__declspec(dllexport) int __cdecl Zimo_SetDepth(int depth)
{
    if (!g_started) return (int)SESSION_NOT_FOUND;
    if (!pSetK3Depth) return (int)FEATURENOTSUPPORTED;
    return (int)pSetK3Depth((UINT)(depth < 0 ? 0 : depth > 62 ? 62 : depth), g_session);
}

__declspec(dllexport) int __cdecl Zimo_PreferredDisplay(void)
{
    if (Zimo_Load() != 0 || !pGetPreferredDisplay) return 0;
    int id = 0;
    return pGetPreferredDisplay(&id) == SUCCESS && id > 0 ? id : 0;
}

__declspec(dllexport) int __cdecl Zimo_GetEyes(ZimoEyes* out)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_haveEyes || !out) return 0;
    *out = g_eyes;
    return 1;
}

__declspec(dllexport) int __cdecl Zimo_TakeStatus(int* act, int* value)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_status.empty()) return 0;
    if (act) *act = g_status.front().first;
    if (value) *value = g_status.front().second;
    g_status.pop_front();
    return 1;
}

__declspec(dllexport) const char* __cdecl Zimo_Describe(int ret)
{
    switch (ret)
    {
    case SUCCESS:             return "success";
    case GENERAL_ERR:         return "general error";
    case INVALID_PARAM:       return "invalid parameter";
    case SESSION_NOT_FOUND:   return "no session";
    case FILE_NOT_FOUND:      return "3DS_SDK.dll not found";
    case TIMEOUT:             return "timed out (is Zondision's 3DS software running?)";
    case UNKOWNPLATFORM:      return "unknown platform";
    case FEATURENOTSUPPORTED: return "this 3D mode is not supported by the display or its firmware";
    case NO_DISPLAY_FOUND:    return "no Zondision display found";
    case INTERNAL_DATA_ERROR: return "the SDK DLL is not the expected version";
    case CONNECTION_ERROR:    return "could not connect to Zondision's 3DS service";
    case LICENSE_EXPIRED:     return "the app key has expired";
    case LICENSE_INVALID:     return "the app key was not accepted";
    default:                  return "unknown error";
    }
}
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID reserved)
{
    // (At unload while the process carries on: the display back to 2D. Not when
    // the process is ending -- the SDK's own threads are gone by then.)
    if (reason == DLL_PROCESS_DETACH && reserved == nullptr) Zimo_Stop();
    return TRUE;
}
