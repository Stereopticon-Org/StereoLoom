// Zondision.h -- Stereo Loom's side of a Zondision 3D display (the Zimo M1): loads
// ZimoAdapter.dll (beside the exe; see adapters/zimo), which holds the app key and
// talks to Zondision's 3DS SDK. The display weaves side by side itself; Stereo Loom
// draws side by side on it (OutputEncoder) and asks for 3D while it does.
#pragma once

#include <windows.h>
#include <string>

namespace srw::Zondision
{
    // How the Zondision device is driven (Settings ZondisionMode; stored as numbers).
    //   EyeTracked     the glasses-free display's own chip weaves, following the eyes:
    //                  side by side, or top and bottom for a top-and-bottom source
    //                  where the display has multiview firmware;
    //   MultiviewTB    many views at once, no eye tracking, from top and bottom
    //                  (multiview firmware);
    //   MultiviewDepth many views at once from a picture and its depth map
    //                  (multiview firmware; an RGB + Depth source);
    //   Shutter / Polarised / AR   Zondision's glasses: one eye a frame, alternate
    //                  rows, or full side by side, with the device put into 3D.
    enum class Mode { EyeTracked, MultiviewTB, MultiviewDepth, Shutter, Polarised, AR, Count };
    const char* ModeName(Mode m);
    // What the encoder draws for the mode in use (OutputEncoder::ZLayout).
    int Layout();

    // ZimoAdapter.dll and Zondision's SDK both load (looked at once).
    bool Available();
    // The modes that can be offered now, one bit each (1 << Mode): from the devices
    // connected, and for the multiview ones what the display said it can do the last
    // time a session ran on it.
    unsigned AvailableModes();
    // The mode for the next session (Start).
    void SetMode(Mode m);
    Mode GetMode();
    // The device for the mode's place on the desktop, matched to a Windows monitor by
    // its name (else its size): the one picked in Zondision's own control panel when it
    // suits the mode, else the first that does. name: the device's own. False: none.
    bool FindDisplay(RECT& out, std::string* name = nullptr);
    // A session with that device (Stop ends it). msg: what happened, for the log.
    bool Start(std::string& msg);
    bool Set3D(bool on);
    void Stop();
    bool IsStarted();
    // EyeTracked: the source is top and bottom -- the display's chip takes it as it is
    // where it can (multiview firmware). True: the display is now taking top and bottom.
    bool SetTopBottom(bool tab);
    bool TopBottom();
    // The multiview modes' 3D strength, 0 to 62 (Zondision's default 16).
    void SetDepth(int depth);
    // Black frames while the display switches between 2D and 3D, which flickers
    // (Settings ZondisionBlackout): on / off, and whether to draw black now.
    void SetBlackoutEnabled(bool on);
    bool Blackout();
    // The viewer's head from the display's eye tracker: position in mm from the
    // display's centre (x right, y up, z towards the viewer), and pitch / yaw /
    // roll in radians -- as SRWeaver::GetHeadPose gives the SR tracker's.
    bool GetHeadPose(double pos[3], double orient[3]);
    // What the display has said since last time: logged and acted on (call every frame).
    void Poll();
}
