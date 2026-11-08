// Zondision.h -- Stereo Loom's side of a Zondision 3D display (the Zimo M1): loads
// ZimoAdapter.dll (beside the exe; see adapters/zimo), which holds the app key and
// talks to Zondision's 3DS SDK. The display weaves side by side itself; Stereo Loom
// draws side by side on it (OutputEncoder) and asks for 3D while it does.
#pragma once

#include <windows.h>
#include <string>

namespace srw::Zondision
{
    // ZimoAdapter.dll and Zondision's SDK both load (looked at once).
    bool Available();
    // The Zondision 3D display's place on the desktop, matched to a Windows monitor
    // by its name (else its size); name: the display's own. False: none connected.
    bool FindDisplay(RECT& out, std::string* name = nullptr);
    // A session with the display (Stop ends it). msg: what happened, for the log.
    bool Start(std::string& msg);
    bool Set3D(bool on);
    void Stop();
    bool IsStarted();
    // The viewer's head from the display's eye tracker: position in mm from the
    // display's centre (x right, y up, z towards the viewer), and pitch / yaw /
    // roll in radians -- as SRWeaver::GetHeadPose gives the SR tracker's.
    bool GetHeadPose(double pos[3], double orient[3]);
    // What the display has said since last time, to the log (call now and then).
    void Poll();
}
