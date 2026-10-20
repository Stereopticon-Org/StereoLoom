// SamsungHub.h -- turning off (and back on) Samsung Odyssey 3D Hub's fullscreen
// "convert to 3D?" popup, which otherwise appears over fullscreen video and games
// on an Odyssey 3D screen while SR Loom is weaving.
//
// The way Hub is asked -- its settings pipe, the message's form and the fields
// it carries, the three preferences that hold the popup -- is from Odyssey
// Stereo Controls by AlonBorodAHub (MIT licence, see THIRD_PARTY_NOTICES.txt):
// https://github.com/AlonBorodAHub/Odyssey-Stereo-Controls
// It is undocumented and was worked out against Hub 1.5.1; a later Hub may
// need changes. Nothing of Samsung's is included or changed beyond the
// preferences Hub itself is asked to save.
#pragma once
#include <string>

namespace SamsungHub
{
    // Hub's settings file is there (Hub has run at least once for this user).
    bool Installed();

    // The popup as Hub's settings have it: 1 on, 0 off, -1 not known.
    int PopupState();

    // Asks Hub to turn the popup off (restore false) or to put back the three
    // preferences saved the first time it was turned off here (restore true),
    // then waits for Hub to have saved it. Hub's pipe may only take an
    // administrator: on "access denied" SR Loom runs itself once elevated to
    // do just this (Windows asks the user). msg: what happened, for the user.
    bool SetPopup(bool restore, std::string& msg);

    // The elevated run's side: SRLoom.exe --hub-popup disable|restore <user SID>.
    // Returns the process's exit code; -1 when the command line is not this.
    int RunElevatedPart(const char* cmdLine);
}
