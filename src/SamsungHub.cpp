// SamsungHub.cpp -- see SamsungHub.h. The pipe, the message and its fields are
// from Odyssey Stereo Controls (AlonBorodAHub, MIT licence).
#include "SamsungHub.h"
#include "Common.h"
#include <windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
    // (A test: SRLOOM_HUB_PIPE names a stand-in pipe, SRLOOM_HUB_SETTINGS a stand-in settings file.)
    const wchar_t* PipeName() { const wchar_t* t = _wgetenv(L"SRLOOM_HUB_PIPE"); return t ? t : L"\\\\.\\pipe\\Odyssey3DHubPipe"; }
    const wchar_t* kKey = L"Software\\SRLoom";
    // The fields Hub's settings message carries, in its order.
    const char* kFields[] = { "CurrOverlayKey", "Curr3DModeKey", "Curr3DDepthNegKey", "Curr3DDepthPosKey", "Curr3DPopoutNegKey",
                              "Curr3DPopoutPosKey", "Language", "CursorType", "IsConvertVideoTo3D", "IsAutoGameScan", "IsAutoRun",
                              "IsAutoConvert", "IsStandardVideo" };
    // The three that hold the popup (saved before the first change, put back on restore).
    const char* kPopupFields[] = { "IsConvertVideoTo3D", "IsAutoConvert", "IsStandardVideo" };

    std::wstring SettingsPath()
    {
        if (const wchar_t* t = _wgetenv(L"SRLOOM_HUB_SETTINGS")) return t;
        wchar_t* prof = nullptr; std::wstring p;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &prof)) && prof)
            p = std::wstring(prof) + L"\\AppData\\LocalLow\\Samsung\\Odyssey3DHubUI\\SettingsData\\SettingsInfo.json";
        CoTaskMemFree(prof);
        return p;
    }

    bool ReadFile(const std::wstring& path, std::string& out)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
        char buf[8192]; size_t n;
        out.clear();
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        fclose(f);
        if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) out.erase(0, 3);
        return true;
    }

    // The "Settings" object's own members, each as the text Hub's message wants
    // (as .NET's Convert.ToString gives it: numbers as written, True / False,
    // strings unquoted). False: no such object.
    bool ReadSettings(std::map<std::string, std::string>& out)
    {
        out.clear();
        std::string js;
        if (!ReadFile(SettingsPath(), js)) return false;
        const size_t k = js.find("\"Settings\"");
        if (k == std::string::npos) return false;
        size_t i = js.find('{', k);
        if (i == std::string::npos) return false;
        ++i;
        auto skipWs = [&]() { while (i < js.size() && (js[i] == ' ' || js[i] == '\t' || js[i] == '\r' || js[i] == '\n' || js[i] == ',')) ++i; };
        auto readString = [&](std::string& s) -> bool {
            if (i >= js.size() || js[i] != '"') return false;
            ++i; s.clear();
            while (i < js.size() && js[i] != '"')
            {
                if (js[i] == '\\' && i + 1 < js.size())
                {
                    const char e = js[++i];
                    s += e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e;   // (\uXXXX kept as written: no field uses it)
                }
                else s += js[i];
                ++i;
            }
            ++i;
            return true;
        };
        for (;;)
        {
            skipWs();
            if (i >= js.size()) return false;
            if (js[i] == '}') return true;
            std::string key, val;
            if (!readString(key)) return false;
            while (i < js.size() && js[i] != ':') ++i;
            ++i;
            while (i < js.size() && (js[i] == ' ' || js[i] == '\t' || js[i] == '\r' || js[i] == '\n')) ++i;
            if (i >= js.size()) return false;
            if (js[i] == '"') { if (!readString(val)) return false; }
            else if (js[i] == '{' || js[i] == '[')
            {
                // (A nested value: skipped -- none of the fields is one.)
                int depth = 0; bool inStr = false;
                for (; i < js.size(); ++i)
                {
                    const char c = js[i];
                    if (inStr) { if (c == '\\') ++i; else if (c == '"') inStr = false; continue; }
                    if (c == '"') inStr = true;
                    else if (c == '{' || c == '[') ++depth;
                    else if ((c == '}' || c == ']') && --depth == 0) { ++i; break; }
                }
                val = "\x01";
            }
            else
            {
                const size_t s0 = i;
                while (i < js.size() && js[i] != ',' && js[i] != '}' && js[i] != '\r' && js[i] != '\n' && js[i] != ' ') ++i;
                val = js.substr(s0, i - s0);
                if (val == "true") val = "True"; else if (val == "false") val = "False"; else if (val == "null") val.clear();
            }
            out[key] = val;
        }
    }

    int AsInt(const std::string& v) { return v == "True" ? 1 : v == "False" ? 0 : atoi(v.c_str()); }

    std::wstring RegString(const wchar_t* name)
    {
        wchar_t buf[512] = {}; DWORD sz = sizeof(buf) - sizeof(wchar_t), type = 0;
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &h) != ERROR_SUCCESS) return L"";
        const LONG r = RegQueryValueExW(h, name, nullptr, &type, (BYTE*)buf, &sz);
        RegCloseKey(h);
        return r == ERROR_SUCCESS && type == REG_SZ ? std::wstring(buf) : std::wstring();
    }
    void RegSetString(const wchar_t* name, const std::wstring& v)
    {
        HKEY h = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return;
        RegSetValueExW(h, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(h);
    }
    std::wstring Wide(const std::string& s) { std::wstring o; for (char c : s) o += (wchar_t)(unsigned char)c; return o; }
    std::string Narrow(const std::wstring& s) { std::string o; for (wchar_t c : s) o += (char)(c < 128 ? c : '?'); return o; }

    std::wstring UserSid()
    {
        HANDLE tok = nullptr; std::wstring out;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return out;
        BYTE buf[256]; DWORD len = 0;
        if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len))
        {
            LPWSTR s = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, &s)) { out = s; LocalFree(s); }
        }
        CloseHandle(tok);
        return out;
    }

    // The change itself. 0 done and confirmed; 1 failed (msg says why); 2 the pipe refused this process.
    int Apply(bool restore, std::string& msg)
    {
        std::map<std::string, std::string> s;
        if (!ReadSettings(s)) { msg = "Samsung Odyssey 3D Hub's settings were not found. Run Hub once, then try again."; return 1; }
        std::map<std::string, std::string> want = s;
        if (restore)
        {
            const std::wstring b = RegString(L"HubPopupBackup");
            if (b.empty()) { msg = "There is no saved Samsung popup preference to put back."; return 1; }
            // (Saved as "IsConvertVideoTo3D=1;IsAutoConvert=1;IsStandardVideo=0".)
            const std::string bs = Narrow(b);
            size_t p = 0;
            while (p < bs.size())
            {
                size_t e = bs.find(';', p); if (e == std::string::npos) e = bs.size();
                const std::string kv = bs.substr(p, e - p); const size_t eq = kv.find('=');
                if (eq != std::string::npos) want[kv.substr(0, eq)] = kv.substr(eq + 1);
                p = e + 1;
            }
        }
        else
        {
            want["IsConvertVideoTo3D"] = "0";
            want["IsAutoConvert"] = "0";
        }
        std::string line = "OH/4/11";
        for (const char* f : kFields)
        {
            auto it = want.find(f);
            if (it == want.end()) { msg = std::string("This version of Samsung Hub is not supported (no ") + f + " setting)."; return 1; }
            if (it->second.find_first_of("/\r\n\x01") != std::string::npos) { msg = "Samsung Hub's settings are in a form Stereo Loom does not know."; return 1; }
            line += "/" + it->second;
        }
        line += "\r\n";
        if (!WaitNamedPipeW(PipeName(), 2500) && GetLastError() == ERROR_FILE_NOT_FOUND) { msg = "Samsung Odyssey 3D Hub is not running. Start Hub, then try again."; return 1; }
        HANDLE pipe = CreateFileW(PipeName(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            const DWORD e = GetLastError();
            if (e == ERROR_ACCESS_DENIED) return 2;
            msg = "Could not reach Samsung Hub (error " + std::to_string(e) + ").";
            return 1;
        }
        // (Saved before the first change only, so a restore always goes back to the user's own choice.)
        if (!restore && RegString(L"HubPopupBackup").empty())
        {
            std::wstring b;
            for (const char* f : kPopupFields) b += (b.empty() ? L"" : L";") + Wide(f) + L"=" + Wide(s[f]);
            RegSetString(L"HubPopupBackup", b);
        }
        DWORD wrote = 0;
        const BOOL ok = WriteFile(pipe, line.data(), (DWORD)line.size(), &wrote, nullptr);
        FlushFileBuffers(pipe);
        CloseHandle(pipe);
        if (!ok || wrote != line.size()) { msg = "Could not send the change to Samsung Hub."; return 1; }
        // Hub saves it to its settings file: wait for that, up to 3 s.
        for (int k = 0; k < 30; ++k)
        {
            Sleep(100);
            std::map<std::string, std::string> now;
            if (!ReadSettings(now)) continue;
            bool same = true;
            for (const char* f : kPopupFields) if (AsInt(now[f]) != AsInt(want[f])) same = false;
            if (same) { msg = restore ? "Samsung's fullscreen 3D popup preference is back as it was." : "Samsung's fullscreen 3D popup is off."; return 0; }
        }
        msg = "Samsung Hub did not confirm the change. Open Hub and try again.";
        return 1;
    }
}

using srw::Log;

namespace
{
    // ---- Hub's games' configs (UEVR's key=value lines) ----
    std::vector<std::wstring> GameConfigs()
    {
        std::vector<std::wstring> out;
        wchar_t* app = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &app)) || !app) { CoTaskMemFree(app); return out; }
        std::wstring root = std::wstring(app) + L"\\3DGameLauncher";
        CoTaskMemFree(app);
        if (const wchar_t* t = _wgetenv(L"SRLOOM_HUB_GAMES")) root = t;   // (a test: stand-in games)
        WIN32_FIND_DATAW fd{};
        HANDLE f = FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) return out;
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            const std::wstring c = root + L"\\" + fd.cFileName + L"\\config.txt";
            if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) out.push_back(c);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
        return out;
    }

    // The file's lines, each with its own ending kept.
    bool ReadLines(const std::wstring& path, std::vector<std::string>& lines)
    {
        std::string all;
        if (!ReadFile(path, all)) return false;
        lines.clear();
        size_t a = 0;
        while (a < all.size()) { size_t e = all.find('\n', a); e = e == std::string::npos ? all.size() : e + 1; lines.push_back(all.substr(a, e - a)); a = e; }
        return true;
    }
    bool WriteAll(const std::wstring& path, const std::vector<std::string>& lines)
    {
        const std::wstring tmp = path + L".srloom-new";
        FILE* f = nullptr;
        if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f) return false;
        bool ok = true;
        for (const std::string& l : lines) ok = ok && fwrite(l.data(), 1, l.size(), f) == l.size();
        ok = (fclose(f) == 0) && ok;
        if (!ok) { DeleteFileW(tmp.c_str()); return false; }
        return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }
    // A line's key, and its value without the line ending ("" when it is not key=value).
    std::string KeyOf(const std::string& l, std::string* val = nullptr)
    {
        const size_t eq = l.find('=');
        if (eq == std::string::npos) return "";
        if (val) { *val = l.substr(eq + 1); while (!val->empty() && (val->back() == '\n' || val->back() == '\r')) val->pop_back(); }
        return l.substr(0, eq);
    }
    std::string Ending(const std::string& l) { return l.size() >= 2 && l[l.size() - 2] == '\r' ? "\r\n" : (!l.empty() && l.back() == '\n') ? "\n" : ""; }
    const char* kSbsKey = "Odyssey3D_ShowSBS";
    const char* kRecKey = "VR_DesktopRecordingFix_V2";
}

namespace SamsungHub
{
    bool Installed()
    {
        const std::wstring p = SettingsPath();
        return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    int PopupState()
    {
        std::map<std::string, std::string> s;
        if (!ReadSettings(s)) return -1;
        auto it = s.find("IsConvertVideoTo3D");
        return it == s.end() ? -1 : (AsInt(it->second) != 0 ? 1 : 0);
    }

    bool SetPopup(bool restore, std::string& msg)
    {
        const int r = Apply(restore, msg);
        if (r != 2) { Log("Samsung Hub popup %s: %s", restore ? "restore" : "off", msg.c_str()); return r == 0; }
        // Hub's pipe takes only an administrator: Stereo Loom once more, elevated, for just this.
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring args = std::wstring(L"--hub-popup ") + (restore ? L"restore " : L"disable ") + UserSid();
        RegSetString(L"HubPopupResult", L"");
        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.fMask = SEE_MASK_NOCLOSEPROCESS; sei.lpVerb = L"runas"; sei.lpFile = exe; sei.lpParameters = args.c_str(); sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei) || !sei.hProcess)
        {
            msg = GetLastError() == ERROR_CANCELLED ? "Administrator approval was cancelled; nothing was changed." : "Could not start the administrator step.";
            Log("Samsung Hub popup: %s", msg.c_str());
            return false;
        }
        WaitForSingleObject(sei.hProcess, 30000);
        DWORD code = 1; GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        msg = Narrow(RegString(L"HubPopupResult"));
        if (msg.empty()) msg = code == 0 ? "Done." : "The administrator step did not finish.";
        Log("Samsung Hub popup %s (as administrator): %s", restore ? "restore" : "off", msg.c_str());
        return code == 0;
    }

    int GamesSbsState(int* games)
    {
        int n = 0, on = 0;
        for (const std::wstring& c : GameConfigs())
        {
            std::vector<std::string> lines;
            if (!ReadLines(c, lines)) continue;
            for (const std::string& l : lines)
            {
                std::string v;
                if (KeyOf(l, &v) != kSbsKey) continue;
                ++n; on += (v == "true" || v == "1") ? 1 : 0;
                break;
            }
        }
        if (games) *games = n;
        return n == 0 ? -1 : (on == n ? 1 : 0);
    }

    bool SetGamesSbs(bool on, std::string& msg)
    {
        int changed = 0, failed = 0, games = 0;
        for (const std::wstring& c : GameConfigs())
        {
            std::vector<std::string> lines;
            if (!ReadLines(c, lines)) { ++failed; continue; }
            bool has = false;
            for (const std::string& l : lines) if (KeyOf(l) == kSbsKey) has = true;
            if (!has) continue;   // (not one of Samsung's 3D games: left alone)
            ++games;
            const std::wstring bak = c + L".srloom";
            std::vector<std::string> saved;
            const bool haveBak = ReadLines(bak, saved);
            if (on && !haveBak)
            {
                // (The game's own two values, kept to put back.)
                std::vector<std::string> keep;
                for (const std::string& l : lines) { const std::string k = KeyOf(l); if (k == kSbsKey || k == kRecKey) keep.push_back(l); }
                if (!WriteAll(bak, keep)) { ++failed; continue; }
            }
            std::map<std::string, std::string> want;
            if (on) { want[kSbsKey] = "true"; want[kRecKey] = "false"; }
            else if (haveBak) { for (const std::string& l : saved) { std::string v; const std::string k = KeyOf(l, &v); if (!k.empty()) want[k] = v; } }
            else want[kSbsKey] = "false";
            bool diff = false;
            for (std::string& l : lines)
            {
                std::string v; const std::string k = KeyOf(l, &v);
                auto it = want.find(k);
                if (it == want.end() || it->second == v) continue;
                // (Only turned off, never added: the recording fix is left as it is where it is not on.)
                if (on && k == kRecKey && v != "true") continue;
                l = k + "=" + it->second + Ending(l);
                diff = true;
            }
            if (diff && !WriteAll(c, lines)) { ++failed; continue; }
            if (!on && haveBak) DeleteFileW(bak.c_str());
            changed += diff ? 1 : 0;
        }
        if (games == 0) msg = "No Samsung 3D games were found (Hub's game launcher keeps a settings file for each game once it has run).";
        else if (failed) msg = "Some games' settings could not be changed (" + std::to_string(failed) + "). Close those games and try again.";
        else msg = on ? "Samsung's 3D games will show side by side for Stereo Loom the next time each starts (" + std::to_string(games) + " found)."
                      : "Samsung's 3D games are back to their own settings from the next time each starts.";
        Log("Samsung Hub games side by side %s: %d games, %d changed, %d failed", on ? "on" : "off", games, changed, failed);
        return games > 0 && failed == 0;
    }

    int RunElevatedPart(const char* cmdLine)
    {
        if (!cmdLine || strncmp(cmdLine, "--hub-popup ", 12) != 0) return -1;
        char what[16] = {}, sid[192] = {};
        if (sscanf_s(cmdLine + 12, "%15s %191s", what, (unsigned)sizeof(what), sid, (unsigned)sizeof(sid)) != 2) return 1;
        std::string msg;
        int r = 1;
        // (The same Windows account must approve: its settings are the ones to change.)
        if (Narrow(UserSid()) != sid) msg = "Approve with the same Windows account that runs Stereo Loom.";
        else
        {
            r = Apply(strcmp(what, "restore") == 0, msg);
            if (r == 2) { msg = "Samsung Hub refused the change even as administrator."; r = 1; }
        }
        RegSetString(L"HubPopupResult", Wide(msg));
        return r;
    }
}
