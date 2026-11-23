// ZimoAdapter.h -- the small C interface between Stereo Loom and ZimoAdapter.dll,
// which talks to a Zondision display (the Zimo M1) through Zondision's 3DS SDK
// (3DS_SDK.dll, redistributed beside it).
//
// Why a separate DLL: Zondision's SDK identifies the calling application by an app
// key, and asks open-source projects to keep that key inside a binary rather than
// in a text file. The adapter is built with the key compiled in (lightly
// obfuscated, see ZimoAdapter.cpp); Stereo Loom itself never sees it.
//
// Stereo Loom loads the DLL at run time (LoadLibrary) and finds these by name.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define ZIMO_ADAPTER_API_VERSION 2

typedef struct
{
    int  id;              // the SDK's display id (from 1)
    char name[32];        // the monitor's own name (EDID)
    int  width, height;   // native pixels
    int  mmWidth, mmHeight;
    int  zondision;       // a Zondision glasses-free display (ZLFD: the Zimo M1)
    int  sr;              // a Leia SR display (Zondision's SDK knows those too; Stereo Loom weaves them itself)
    char serial[16];
    int  shutter;         // a Zondision shutter-glasses monitor
    int  polarized;       // a Zondision polarised-glasses monitor
    int  ar;              // Zondision AR glasses
} ZimoDisplay;

typedef struct
{
    int   faces;          // faces seen (0: no viewer)
    float left[3];        // smoothed eye positions, display-centred, right handed, y up (the SDK's units)
    float right[3];
    float headRotation[3];   // pitch, yaw, roll, degrees
    unsigned serial;      // goes up with each sample
} ZimoEyes;

// The adapter's interface version (ZIMO_ADAPTER_API_VERSION).
typedef int  (__cdecl* Zimo_ApiVersionFn)(void);
// Loads 3DS_SDK.dll (beside the adapter first, then the usual search). 0: loaded.
typedef int  (__cdecl* Zimo_LoadFn)(void);
// The 3D displays the SDK knows of: how many (up to max are filled in).
typedef int  (__cdecl* Zimo_ListDisplaysFn)(ZimoDisplay* out, int max);
// The ways the display can be driven: the SDK's WeavingMode values.
enum { ZIMO_SBS = 2, ZIMO_TB = 3, ZIMO_RGBDEPTH = 4, ZIMO_MULTIVIEW_TB = 5, ZIMO_AR = 6, ZIMO_POLARIZED = 7, ZIMO_SHUTTER = 8 };
// What the display can do (the SDK's PlatformFeatures): 0 GPU weaving only, 1 its own
// chip weaves (eye-tracked side by side), 2 multiview firmware too (top and bottom,
// multiview, picture + depth).
// A session with display `id` (0: the SDK's choice) in `mode` (ZIMO_...). 0: started;
// else the SDK's error (Zimo_Describe). features (may be null) gets what the display
// can do; sdkVersion (may be null) the SDK's version text.
typedef int  (__cdecl* Zimo_StartFn)(int id, int mode, int* features, char* sdkVersion, int sdkVersionSize);
// Another mode for the running session (ZIMO_...): taken at once if 3D is on. 0: done.
typedef int  (__cdecl* Zimo_SetModeFn)(int mode);
// The multiview modes' 3D strength, 0 to 62 (Zondision's default 16). 0: done.
typedef int  (__cdecl* Zimo_SetDepthFn)(int depth);
// The 3D display picked in Zondision's own control panel (its id), 0 if none / unknown.
typedef int  (__cdecl* Zimo_PreferredDisplayFn)(void);
// 3D on (1) or off (0, 2D). 0: done, else the SDK's error.
typedef int  (__cdecl* Zimo_Set3DFn)(int on);
// The latest eye positions; 0 until the first.
typedef int  (__cdecl* Zimo_GetEyesFn)(ZimoEyes* out);
// Status the display sent since the last call, one at a time (the SDK's ACTLIST
// value and its number); 0 when there is none left.
typedef int  (__cdecl* Zimo_TakeStatusFn)(int* act, int* value);
// The session ended (2D first).
typedef void (__cdecl* Zimo_StopFn)(void);
// An SDK error number in words.
typedef const char* (__cdecl* Zimo_DescribeFn)(int ret);
// Which key the adapter was built with: 0 Zondision's evaluation key (which
// expires), 1 Stereo Loom's own.
typedef int  (__cdecl* Zimo_KeyKindFn)(void);

#ifdef __cplusplus
}
#endif
