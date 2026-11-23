// UpdateChecker.cpp — WinHTTP poll of GitHub's releases, and the in-place update.
// The network work runs on detached threads; we never block the main loop.
//
// JSON parsing is deliberately substring-only. The fields we want (tag_name,
// html_url, prerelease, and the StereoLoom.exe asset's url / size / digest) are plain
// values; pulling in a JSON dependency for that would be overkill. If GitHub ever
// changes the shape we'll just stop detecting updates, which is the safe failure mode.
//
// The update itself: the release's StereoLoom.exe is downloaded beside this one as
// StereoLoom.exe.new and checked (its size and SHA-256 against GitHub's, its version
// resource against the release's tag). This exe is then renamed StereoLoom.exe.old (a
// running exe can be renamed, not overwritten), the new one takes its name and is
// started, and this one quits. The new one waits for the old to close, then deletes
// StereoLoom.exe.old. Anything failing on the way puts the old one back.

#include "UpdateChecker.h"

#include "Common.h"
#include "Settings.h"
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

using srw::Log;

namespace
{
    // Registry path for the throttle timestamp and the cached release (sibling of
    // Settings.cpp's keys -- kept inline so UpdateChecker is self-contained).
    constexpr wchar_t kSettingsKey[]      = L"Software\\SRLoom";
    constexpr wchar_t kLastCheckValue[]   = L"LastUpdateCheckUnix";
    constexpr char    kLatestTagValue[]   = "LatestReleaseTag";
    constexpr char    kLatestUrlValue[]   = "LatestReleaseUrl";
    constexpr char    kLatestAssetValue[] = "LatestReleaseAsset";
    constexpr char    kLatestSizeValue[]  = "LatestReleaseSize";
    constexpr char    kLatestShaValue[]   = "LatestReleaseSha256";
    constexpr char    kLatestPreValue[]   = "LatestReleaseBeta";
    // 1 hour: checks also run when the panel is opened, so this is the real cap.
    // GitHub's unauthenticated API allows 60 requests/hour per IP; we make at most 1.
    constexpr DWORD   kThrottleSeconds    = 60 * 60;

    std::atomic<int> g_progress{ -1 };

    std::string ReadRegString(const char* name)
    {
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &h) != ERROR_SUCCESS)
            return {};
        char buf[1024] = {};
        DWORD sz = sizeof(buf) - 1, type = 0;
        std::string out;
        if (RegQueryValueExA(h, name, nullptr, &type,
                             reinterpret_cast<BYTE*>(buf), &sz) == ERROR_SUCCESS && type == REG_SZ)
            out = buf;
        RegCloseKey(h);
        return out;
    }

    void WriteRegString(const char* name, const std::string& value)
    {
        HKEY h = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                            KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS)
            return;
        RegSetValueExA(h, name, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(value.c_str()), (DWORD)value.size() + 1);
        RegCloseKey(h);
    }

    // Read the throttle timestamp; 0 if never recorded.
    DWORD ReadLastCheckUnix()
    {
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &h) != ERROR_SUCCESS)
            return 0;
        DWORD v = 0, sz = sizeof(v), type = REG_DWORD;
        if (RegQueryValueExW(h, kLastCheckValue, nullptr, &type,
                             reinterpret_cast<BYTE*>(&v), &sz) != ERROR_SUCCESS
            || type != REG_DWORD)
            v = 0;
        RegCloseKey(h);
        return v;
    }

    void WriteLastCheckUnix(DWORD value)
    {
        HKEY h = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                            KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS)
            return;
        RegSetValueExW(h, kLastCheckValue, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(h);
    }

    // Seconds since the Unix epoch (sufficient through 2106 in a DWORD).
    DWORD UnixNow()
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u;
        u.LowPart  = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        constexpr ULONGLONG kEpochDiff100ns = 116444736000000000ULL;
        return static_cast<DWORD>((u.QuadPart - kEpochDiff100ns) / 10000000ULL);
    }

    // The value of `"field": ...` first found in json[from, to): a string's contents
    // (escapes left as they are: tags, URLs and digests have none), or a bare value
    // (number / true / false / null) as written. False when it is not there.
    bool FieldIn(const std::string& j, size_t from, size_t to, const char* field,
                 std::string& out, size_t* at = nullptr)
    {
        const std::string needle = std::string("\"") + field + "\"";
        size_t i = from;
        while (true)
        {
            i = j.find(needle, i);
            if (i == std::string::npos || i >= to) return false;
            size_t k = i + needle.size();
            while (k < j.size() && (j[k] == ' ' || j[k] == '\n' || j[k] == '\r' || j[k] == '\t')) ++k;
            if (k < j.size() && j[k] == ':') { i = k + 1; break; }
            i = k;   // (the name appeared as a value; keep looking)
        }
        while (i < j.size() && (j[i] == ' ' || j[i] == '\n' || j[i] == '\r' || j[i] == '\t')) ++i;
        if (i >= j.size()) return false;
        if (at) *at = i;
        if (j[i] == '"')
        {
            size_t e = i + 1;
            while (e < j.size() && j[e] != '"') e += (j[e] == '\\') ? 2 : 1;
            if (e >= j.size()) return false;
            out = j.substr(i + 1, e - i - 1);
        }
        else
        {
            size_t e = i;
            while (e < j.size() && j[e] != ',' && j[e] != '}' && j[e] != ']' && j[e] != ' ' && j[e] != '\n' && j[e] != '\r') ++e;
            out = j.substr(i, e - i);
        }
        return true;
    }

    // A version's rank, packed as the exe's version resource is (Settings::ExeVersion):
    // major.minor.patch.build, 16 bits each, build the beta's number or 65535 for a
    // release. From a tag ("v4.1", "v4.0.1", "v4.2-beta.14").
    unsigned long long RankOf(const std::string& raw)
    {
        std::string s = (!raw.empty() && (raw[0] == 'v' || raw[0] == 'V')) ? raw.substr(1) : raw;
        unsigned long long part[3] = { 0, 0, 0 };
        size_t i = 0;
        for (int n = 0; n < 3; ++n)
        {
            unsigned long long v = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
            part[n] = v > 0xFFFF ? 0xFFFF : v;
            if (i < s.size() && s[i] == '.') ++i; else break;
        }
        unsigned long long build = 0xFFFF;
        std::string lower = s;
        for (char& c : lower) c = (char)tolower((unsigned char)c);
        const size_t b = lower.find("beta", i);
        if (b != std::string::npos)
        {
            size_t k = b + 4;
            while (k < lower.size() && (lower[k] < '0' || lower[k] > '9')) ++k;
            unsigned long long v = 0;
            while (k < lower.size() && lower[k] >= '0' && lower[k] <= '9') v = v * 10 + (lower[k++] - '0');
            build = v > 0xFFFE ? 0xFFFE : v;
        }
        return part[0] << 48 | part[1] << 32 | part[2] << 16 | build;
    }

    constexpr unsigned long long kCurrentRank =
        (unsigned long long)SRLOOM_VERSION_MAJOR << 48 | (unsigned long long)SRLOOM_VERSION_MINOR << 32 |
        (unsigned long long)SRLOOM_VERSION_PATCH << 16 | (unsigned long long)SRLOOM_VERSION_BUILD;

    std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

    std::wstring SelfPath()
    {
        wchar_t p[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
        return (n && n < MAX_PATH) ? std::wstring(p) : std::wstring();
    }

    // HTTPS GET of `url`, its body handed to `sink` in pieces (false from it stops).
    // `total` gets the Content-Length (0 if not given). True on 200 with all of it read.
    template <class Sink>
    bool HttpGet(const std::wstring& url, const wchar_t* headers, unsigned long long& total, Sink sink)
    {
        total = 0;
        URL_COMPONENTS uc{ sizeof(uc) };
        wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
        uc.lpszHostName = host;  uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path;   uc.dwUrlPathLength = 2048;
        uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
        const std::wstring object = std::wstring(path) + extra;

        HINTERNET hSession = WinHttpOpen(L"StereoLoom-Updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return false;
        WinHttpSetTimeouts(hSession, 10000, 10000, 30000, 30000);
        HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
        HINTERNET hRequest = hConnect ? WinHttpOpenRequest(hConnect, L"GET", object.c_str(), nullptr,
                                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                          WINHTTP_FLAG_SECURE) : nullptr;
        bool ok = false;
        // (GitHub's release downloads redirect to its file host: WinHTTP follows that.)
        if (hRequest && WinHttpSendRequest(hRequest, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS,
                                           headers ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            && WinHttpReceiveResponse(hRequest, nullptr))
        {
            DWORD status = 0, sz = sizeof(status);
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX)
                && status == 200)
            {
                wchar_t len[32] = {}; DWORD lsz = sizeof(len);
                if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                        len, &lsz, WINHTTP_NO_HEADER_INDEX))
                    total = _wcstoui64(len, nullptr, 10);
                std::vector<char> buf(64 * 1024);
                ok = true;
                DWORD available = 0;
                while (true)
                {
                    if (!WinHttpQueryDataAvailable(hRequest, &available)) { ok = false; break; }
                    if (available == 0) break;
                    if (buf.size() < available) buf.resize(available);
                    DWORD read = 0;
                    if (!WinHttpReadData(hRequest, buf.data(), available, &read)) { ok = false; break; }
                    if (read == 0) break;
                    if (!sink(buf.data(), read)) { ok = false; break; }
                }
            }
            else Log("Update: HTTP %lu from the server", status);
        }
        if (hRequest) WinHttpCloseHandle(hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return ok;
    }

    // The newest release in GitHub's release list that this copy may take: betas
    // only when `betas`. False if there is none (or the list could not be read).
    bool NewestRelease(const std::string& j, bool betas, srw::ReleaseInfo& best)
    {
        bool found = false;
        unsigned long long bestRank = 0;
        // Each release object holds exactly one "assets_url" (assets do not), so the
        // list splits there.
        std::vector<size_t> starts;
        for (size_t p = j.find("\"assets_url\""); p != std::string::npos; p = j.find("\"assets_url\"", p + 1))
            starts.push_back(p);
        for (size_t r = 0; r < starts.size(); ++r)
        {
            const size_t from = starts[r], to = (r + 1 < starts.size()) ? starts[r + 1] : j.size();
            std::string tag, url, pre, draft;
            if (!FieldIn(j, from, to, "tag_name", tag) || tag.empty()) continue;
            FieldIn(j, from, to, "html_url", url);   // (the release's: before its author's)
            FieldIn(j, from, to, "prerelease", pre);
            FieldIn(j, from, to, "draft", draft);
            if (draft == "true") continue;
            if (pre == "true" && !betas) continue;
            const unsigned long long rank = RankOf(tag);
            if (found && rank <= bestRank) continue;
            srw::ReleaseInfo info;
            info.tag = tag;
            info.url = url;
            // The StereoLoom.exe asset: its fields up to its browser_download_url (an
            // asset's last field), so none are taken from the next asset.
            for (size_t p = from; ; )
            {
                std::string name; size_t at = 0;
                if (!FieldIn(j, p, to, "name", name, &at)) break;
                p = at + 1;
                if (_stricmp(name.c_str(), "StereoLoom.exe") != 0) continue;
                std::string dl, size, digest; size_t dlAt = 0;
                if (!FieldIn(j, at, to, "browser_download_url", dl, &dlAt)) break;
                if (FieldIn(j, at, dlAt, "size", size)) info.assetSize = _strtoui64(size.c_str(), nullptr, 10);
                if (FieldIn(j, at, dlAt, "digest", digest) && digest.rfind("sha256:", 0) == 0)
                    info.assetSha256 = digest.substr(7);
                info.assetUrl = dl;
                break;
            }
            best = info;
            bestRank = rank;
            found = true;
        }
        return found;
    }

    // Is `url` one of this repository's release downloads?
    bool IsOurDownload(const std::string& url)
    {
        const std::string prefix = std::string("https://github.com/") + srw::kRepoSlug + "/releases/download/";
        return _strnicmp(url.c_str(), prefix.c_str(), prefix.size()) == 0;
    }

    bool DownloadAndCheck(const srw::ReleaseInfo& rel)
    {
        if (!IsOurDownload(rel.assetUrl)) { Log("Update: not one of our release downloads: %s", rel.assetUrl.c_str()); return false; }
        const std::wstring self = SelfPath();
        if (self.empty()) return false;
        const std::wstring newPath = self + L".new";
        HANDLE f = CreateFileW(newPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE)
        {
            Log("Update: cannot write beside the exe (%ls), error %lu", newPath.c_str(), GetLastError());
            return false;
        }
        BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0
            || BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0)
        {
            if (alg) BCryptCloseAlgorithmProvider(alg, 0);
            CloseHandle(f); DeleteFileW(newPath.c_str());
            return false;
        }
        unsigned long long written = 0, total = 0;
        const unsigned long long expect = rel.assetSize;
        bool writeOk = true;
        const bool got = HttpGet(Widen(rel.assetUrl), nullptr, total, [&](const char* d, DWORD n)
        {
            DWORD w = 0;
            if (!WriteFile(f, d, n, &w, nullptr) || w != n) { writeOk = false; return false; }
            BCryptHashData(hash, (PUCHAR)d, n, 0);
            written += n;
            if (written > 300ull * 1024 * 1024) return false;   // (no exe of ours is near this)
            const unsigned long long of = expect ? expect : total;
            if (of) g_progress = (int)(written * 100 / of > 99 ? 99 : written * 100 / of);
            return true;
        });
        UCHAR digest[32] = {};
        BCryptFinishHash(hash, digest, sizeof(digest), 0);
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        CloseHandle(f);

        auto fail = [&](const char* why) { Log("Update: %s", why); DeleteFileW(newPath.c_str()); return false; };
        if (!got || !writeOk) return fail("the download failed");
        if (expect && written != expect) return fail("the download is the wrong size");
        if (!rel.assetSha256.empty())
        {
            char hex[65] = {};
            for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
            if (_stricmp(hex, rel.assetSha256.c_str()) != 0) return fail("the download's SHA-256 does not match GitHub's");
        }
        const unsigned long long ver = srw::Settings::ExeVersion(newPath);
        if (ver != RankOf(rel.tag))
        {
            Log("Update: the downloaded exe's version (%016llx) is not %s's (%016llx)", ver, rel.tag.c_str(), RankOf(rel.tag));
            return fail("the downloaded exe is not the release's");
        }
        Log("Update: %s downloaded and checked (%llu bytes)", rel.tag.c_str(), written);
        return true;
    }
}

namespace srw::UpdateChecker
{
    std::string DisplayVersion(const std::string& tag)
    {
        std::string s = (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) ? tag.substr(1) : tag;
        const size_t b = s.find("-beta");
        if (b == std::string::npos) return s;
        std::string n;
        for (size_t k = b + 5; k < s.size(); ++k) if (s[k] >= '0' && s[k] <= '9') n += s[k];
        return s.substr(0, b) + " beta " + n;
    }

    void StartAsync(HWND hwnd, UINT notifyMsg, bool force)
    {
        if (!hwnd) return;

        // Throttle: skip if we checked within the last kThrottleSeconds -- unless
        // this is a user-forced check, where the whole point is to refresh on demand.
        if (!force)
        {
            const DWORD now  = UnixNow();
            const DWORD last = ReadLastCheckUnix();
            if (last != 0 && now > last && (now - last) < kThrottleSeconds)
            {
                Log("UpdateChecker: skipping (last check %u s ago, throttle %u s)",
                    (unsigned)(now - last), (unsigned)kThrottleSeconds);
                return;
            }
        }
        const bool betas = Settings::ReadGetBetas();

        std::thread([hwnd, notifyMsg, force, betas]()
        {
            auto post = [&](ReleaseInfo* info) {
                if (!PostMessageW(hwnd, notifyMsg, reinterpret_cast<WPARAM>(info), 0))
                    delete info;
            };
            auto failed = [] { auto* i = new ReleaseInfo; i->status = ReleaseInfo::Failed; i->forced = true; return i; };

            const std::wstring url = Widen(std::string("https://api.github.com/repos/") + kRepoSlug + "/releases?per_page=10");
            std::string body;
            unsigned long long total = 0;
            const bool got = HttpGet(url, L"Accept: application/vnd.github+json\r\n", total,
                                     [&](const char* d, DWORD n) { body.append(d, n); return body.size() < 2 * 1024 * 1024; });
            if (!got || body.empty())
            {
                Log("UpdateChecker: fetch failed");
                if (force) post(failed());
                return;
            }
            // Record the check timestamp regardless of result so we don't re-hit the
            // API on every launch even if no release exists yet.
            WriteLastCheckUnix(UnixNow());

            ReleaseInfo best;
            if (!NewestRelease(body, betas, best))
            {
                Log("UpdateChecker: no release in the response (len=%zu)", body.size());
                if (force) post(failed());
                return;
            }
            // Remember it, so the next launch can show the update notice without
            // waiting for a network check.
            WriteRegString(kLatestTagValue, best.tag);
            WriteRegString(kLatestUrlValue, best.url);
            WriteRegString(kLatestAssetValue, best.assetUrl);
            WriteRegString(kLatestSizeValue, std::to_string(best.assetSize));
            WriteRegString(kLatestShaValue, best.assetSha256);
            WriteRegString(kLatestPreValue, betas ? "1" : "0");

            const bool newer = RankOf(best.tag) > kCurrentRank;
            Log("UpdateChecker: current=%s latest=%s%s newer=%d", kAppVersionDisplay, best.tag.c_str(),
                betas ? " (betas included)" : "", newer ? 1 : 0);
            if (!newer)
            {
                if (force) { auto* i = new ReleaseInfo(best); i->status = ReleaseInfo::UpToDate; i->forced = true; post(i); }
                return;
            }
            auto* i = new ReleaseInfo(best);
            i->status = ReleaseInfo::Available;
            i->forced = force;
            post(i);
        }).detach();
    }

    bool CachedUpdate(ReleaseInfo& info)
    {
        info = ReleaseInfo{};
        info.tag = ReadRegString(kLatestTagValue);
        info.url = ReadRegString(kLatestUrlValue);
        info.assetUrl = ReadRegString(kLatestAssetValue);
        info.assetSize = _strtoui64(ReadRegString(kLatestSizeValue).c_str(), nullptr, 10);
        info.assetSha256 = ReadRegString(kLatestShaValue);
        const bool fromBetas = ReadRegString(kLatestPreValue) == "1";
        if (info.tag.empty() || info.url.empty() || RankOf(info.tag) <= kCurrentRank
            || fromBetas != Settings::ReadGetBetas())
        {
            info = ReleaseInfo{};
            return false;
        }
        return true;
    }

    void ResetCache()
    {
        WriteLastCheckUnix(0);
        WriteRegString(kLatestTagValue, "");
    }

    void DownloadAsync(HWND hwnd, UINT notifyMsg, const ReleaseInfo& release)
    {
        int idle = -1;
        if (!g_progress.compare_exchange_strong(idle, 0)) return;   // (one already running)
        std::thread([hwnd, notifyMsg, release]()
        {
            const bool ok = DownloadAndCheck(release);
            g_progress = -1;
            PostMessageW(hwnd, notifyMsg, ok ? 1 : 0, 0);
        }).detach();
    }

    int DownloadProgress() { return g_progress.load(); }

    bool InstallAndRestart()
    {
        const std::wstring self = SelfPath();
        if (self.empty()) return false;
        const std::wstring oldPath = self + L".old", newPath = self + L".new";
        DeleteFileW(oldPath.c_str());
        if (!MoveFileExW(self.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            Log("Update: could not rename the running exe (error %lu)", GetLastError());
            return false;
        }
        if (!MoveFileExW(newPath.c_str(), self.c_str(), 0))
        {
            Log("Update: could not move the new exe into place (error %lu)", GetLastError());
            MoveFileExW(oldPath.c_str(), self.c_str(), 0);
            return false;
        }
        std::wstring cmd = L"\"" + self + L"\" --after-update " + std::to_wstring(GetCurrentProcessId());
        std::wstring dir = self.substr(0, self.find_last_of(L"\\/"));
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(self.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
        {
            Log("Update: could not start the new exe (error %lu) -- putting the old one back", GetLastError());
            MoveFileExW(self.c_str(), newPath.c_str(), MOVEFILE_REPLACE_EXISTING);
            MoveFileExW(oldPath.c_str(), self.c_str(), 0);
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Log("Update: the new exe started (pid %lu); closing this one", pi.dwProcessId);
        return true;
    }

    bool CleanUpOld()
    {
        const std::wstring self = SelfPath();
        if (self.empty()) return true;
        bool done = true;
        for (const wchar_t* ext : { L".old", L".new" })
        {
            const std::wstring p = self + ext;
            if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            if (DeleteFileW(p.c_str())) Log("Update: removed %ls", p.c_str());
            else done = false;
        }
        return done;
    }

    void WaitForUpdatedFrom(const char* cmdLine)
    {
        const char* a = cmdLine ? strstr(cmdLine, "--after-update ") : nullptr;
        if (!a) return;
        const DWORD pid = strtoul(a + 15, nullptr, 10);
        HANDLE p = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
        if (!p) return;
        Log("Update: waiting for the previous version (pid %lu) to close", pid);
        WaitForSingleObject(p, 20000);
        CloseHandle(p);
    }
}
