// updtest -- the update checker and in-place update, off the screen:
//   updtest check     the newest release on GitHub (as Stereo Loom sees it), then its
//                     exe downloaded and checked (StereoLoom.exe.new beside this exe)
//   updtest swap      this exe swapped for a copy of itself, as an update does: the
//                     copy is started, waits for this one to close, removes the old
//                     one, and writes the outcome to updtest.result
// Not part of the app:  cmake --build build/x64 --config Release --target updtest
#include "Common.h"
#include "UpdateChecker.h"
#include "Settings.h"
#include <cstdio>

using namespace srw;

static int g_fail = 0;
static void Expect(bool ok, const char* what) { printf("%s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }

static UINT WaitMsg(UINT msg, WPARAM& wp, DWORD ms)
{
    const DWORD end = GetTickCount() + ms;
    MSG m;
    while (GetTickCount() < end)
    {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            if (m.message == msg) { wp = m.wParam; return msg; }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
    }
    return 0;
}

int main(int argc, char** argv)
{
    std::string cmdLine;
    for (int i = 1; i < argc; ++i) { cmdLine += argv[i]; cmdLine += ' '; }
    const char* mode = argc > 1 ? argv[1] : "check";

    if (strstr(cmdLine.c_str(), "--after-update"))
    {
        // The started copy: as WinMain does.
        UpdateChecker::WaitForUpdatedFrom(cmdLine.c_str());
        bool gone = UpdateChecker::CleanUpOld();
        for (int i = 0; i < 20 && !gone; ++i) { Sleep(500); gone = UpdateChecker::CleanUpOld(); }
        FILE* f = fopen(ExePath("updtest.result").c_str(), "w");
        if (f) { fprintf(f, gone ? "old removed\n" : "old still there\n"); fclose(f); }
        return 0;
    }

    Expect(UpdateChecker::DisplayVersion("v4.2-beta.14") == "4.2 beta 14", "DisplayVersion beta");
    Expect(UpdateChecker::DisplayVersion("v4.1") == "4.1", "DisplayVersion release");

    if (strcmp(mode, "swap") == 0)
    {
        wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
        const std::wstring newPath = std::wstring(self) + L".new";
        Expect(CopyFileW(self, newPath.c_str(), FALSE) != 0, "copy of this exe as .new");
        DeleteFileA(ExePath("updtest.result").c_str());
        Expect(UpdateChecker::InstallAndRestart(), "swapped in and started");
        printf("(the started copy writes updtest.result)\n");
        return g_fail;
    }

    HWND wnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    const UINT kResult = WM_APP + 7, kDone = WM_APP + 13;
    UpdateChecker::StartAsync(wnd, kResult, true);
    WPARAM wp = 0;
    Expect(WaitMsg(kResult, wp, 20000) == kResult && wp, "check answered");
    if (!wp) return 1;
    ReleaseInfo* r = reinterpret_cast<ReleaseInfo*>(wp);
    printf("status %d  betas %d\n  tag   %s (%s)\n  page  %s\n  exe   %s\n  size  %llu\n  sha   %s\n",
           (int)r->status, Settings::ReadGetBetas() ? 1 : 0, r->tag.c_str(), UpdateChecker::DisplayVersion(r->tag).c_str(),
           r->url.c_str(), r->assetUrl.c_str(), r->assetSize, r->assetSha256.c_str());
    Expect(!r->tag.empty() && !r->assetUrl.empty() && r->assetSize > 0, "release has its exe");
    UpdateChecker::DownloadAsync(wnd, kDone, *r);
    int last = -2;
    const DWORD end = GetTickCount() + 120000;
    MSG m; bool done = false; WPARAM ok = 0;
    while (!done && GetTickCount() < end)
    {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) if (m.message == kDone) { done = true; ok = m.wParam; }
        const int p = UpdateChecker::DownloadProgress();
        if (p / 25 != last / 25 && p >= 0) { printf("  %d%%\n", p); last = p; }
        Sleep(50);
    }
    printf("download result %d (1 ready to install; see srweaver.log for why not)\n", (int)ok);
    delete r;
    return g_fail;
}
