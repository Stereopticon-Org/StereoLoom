// version.h -- Stereo Loom's version, in one place: kAppVersion (Common.h, the update
// check), the exe's version resource (app.rc, which a newer copy reads to take over
// from an older one), and the release workflow's check against the tag.
// Bump with each release; SRLOOM_VERSION_STR is the tag without its "v".
#pragma once

#define SRLOOM_VERSION_MAJOR 4
#define SRLOOM_VERSION_MINOR 1
#define SRLOOM_VERSION_PATCH 0
#define SRLOOM_VERSION_STR   "4.1"
