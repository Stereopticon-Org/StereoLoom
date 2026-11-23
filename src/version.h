// version.h -- Stereo Loom's version, in one place: kAppVersion (Common.h, the update
// check), the exe's version resource (app.rc, which a newer copy reads to take over
// from an older one), and the release workflows' check against the tag.
// Bump with each release; SRLOOM_VERSION_STR is the tag without its "v". Right after
// a release, bump it to the next version, so the betas built from main are numbered
// for that one ("4.2 beta 14": newer than 4.1, older than 4.2).
#pragma once

#define SRLOOM_VERSION_MAJOR 4
#define SRLOOM_VERSION_MINOR 1
#define SRLOOM_VERSION_PATCH 0
#define SRLOOM_VERSION_STR   "4.1"

// A beta's number (the beta workflow's run number: cmake -DSRLOOM_BETA=N); 0 for a
// release. The exe's fourth version field: the beta's number, or 65535 for a release,
// so a release ranks above every beta of the same version.
#ifndef SRLOOM_VERSION_BETA
#define SRLOOM_VERSION_BETA 0
#endif
#if SRLOOM_VERSION_BETA
#define SRLOOM_VERSION_BUILD SRLOOM_VERSION_BETA
#else
#define SRLOOM_VERSION_BUILD 65535
#endif
