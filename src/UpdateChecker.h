// UpdateChecker.h — background poll of GitHub Releases for a newer Stereo Loom build,
// and the in-place update (download the release's StereoLoom.exe, swap it in, restart).
#pragma once

#include <windows.h>
#include <string>

namespace srw
{
    // Result of an update check. Heap-allocated and handed to the main
    // thread via PostMessage; the receiver owns it and must `delete` it.
    struct ReleaseInfo
    {
        enum Status
        {
            Available,   // a release newer than this build was found
            UpToDate,    // nothing newer (user-triggered check only)
            Failed,      // network / parse failure (user-triggered check only)
        };
        Status      status = Available;
        std::string tag;   // e.g. "v4.2" or "v4.2-beta.14" -- populated when Available or UpToDate
        std::string url;   // GitHub release page (HTML) -- populated when Available
        bool        forced = false;   // user-triggered check (always wants feedback)
        // The release's StereoLoom.exe (empty if it has none): where to download it,
        // its size, and its SHA-256 as GitHub reports it (hex; empty if not given).
        std::string        assetUrl;
        unsigned long long assetSize = 0;
        std::string        assetSha256;
    };

    namespace UpdateChecker
    {
        // Kick off an asynchronous check against the GitHub Releases API.
        // Posts `notifyMsg` to `hwnd` with `wParam = (WPARAM)(ReleaseInfo*)`
        // when the check completes. Betas (pre-releases) count only when
        // Settings::ReadGetBetas() is on.
        //
        // force=false (auto-check): respects a once-per-hour throttle stored
        // in HKCU\Software\SRLoom\LastUpdateCheckUnix to avoid hammering
        // api.github.com, and only posts when a newer release is found
        // (silent on up-to-date / failure -- we don't want to nag on launch).
        //
        // force=true (user-triggered, e.g. "Check for updates" button):
        // skips the throttle, and posts a ReleaseInfo for ALL outcomes
        // (Available / UpToDate / Failed) so the GUI can show feedback.
        void StartAsync(HWND hwnd, UINT notifyMsg, bool force = false);

        // The newest release seen by the LAST successful check (cached in
        // HKCU\Software\SRLoom), if it's newer than this build. Lets the GUI
        // + tray show "update available" straight away on launch, even when
        // the throttle skips the network check. False if none / up to date.
        bool CachedUpdate(ReleaseInfo& info);

        // Forget the cached release and the throttle (Get Betas switched: the next
        // check looks again at once).
        void ResetCache();

        // "v4.2-beta.14" -> "4.2 beta 14"; "v4.1" -> "4.1".
        std::string DisplayVersion(const std::string& tag);

        // Download the release's exe beside this one (StereoLoom.exe.new) on a worker
        // thread, and check it: its size, its SHA-256, and its version resource being
        // the release's. Posts `notifyMsg` to `hwnd` with wParam 1 when it is ready to
        // install, 0 when it failed (the reason is logged).
        void DownloadAsync(HWND hwnd, UINT notifyMsg, const ReleaseInfo& release);
        // How far the download is: 0-100, or -1 when none is running.
        int  DownloadProgress();

        // Swap the downloaded exe in (this one renamed StereoLoom.exe.old, the new one
        // to this one's name) and start it; it waits for this one to close. False (with
        // everything put back) if that could not be done. The caller then quits.
        bool InstallAndRestart();

        // At startup: removes the old exe a previous update left behind (and a
        // download that never got installed). False while it is still there (its
        // process not yet gone): call again later.
        bool CleanUpOld();

        // "--after-update <pid>" on the command line (InstallAndRestart's restart):
        // waits for that process (the old copy) to close.
        void WaitForUpdatedFrom(const char* cmdLine);
    }
}
