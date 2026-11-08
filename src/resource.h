// resource.h — resource identifiers.
#pragma once

#define IDI_TRAY 101          // (the app icon: the dark tile -- the lowest id, so Explorer shows it)
#define IDI_APP_LIGHT 102     // (the light tile, carried in the exe as well)
#define IDI_TRAY_ON_DARK  103   // (the tray's: the mark for a dark taskbar)
#define IDI_TRAY_ON_LIGHT 104   // (... and for a light one)

// Embedded UI fonts (Inter, SIL OFL) so the release is a single self-contained exe.
#define IDR_FONT_REGULAR  201
#define IDR_FONT_SEMIBOLD 202

// Embedded NPClient.dll (x64) for TrackIR-aware games. Extracted to
// %LOCALAPPDATA%\SRLoom\NPClient\ on first launch.
#define IDR_NPCLIENT64    301

// Zondision (built where its SDK is present): ZimoAdapter.dll and the SDK's two DLLs,
// carried in the exe so the release stays a single file. Unpacked on first use to
// %LOCALAPPDATA%\SRLoom\Zondision\<hash>\ (Zondision.cpp).
#define IDR_ZIMO_ADAPTER  401
#define IDR_ZIMO_SDK      402
#define IDR_ZIMO_JSONCPP  403
