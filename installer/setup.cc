// Fallout 1 Realtime -- the Windows installer.
//
// One exe that carries the game (and data\aimmask.dat) inside as resources and puts them into an
// existing Fallout 1. That is the whole job:
//
//  * It finds the Steam copy (app 38400) through the registry and libraryfolders.vdf, so a library
//    on another drive is found too, or it takes a folder you pick.
//  * STEAM: Steam's Play button runs FalloutLauncher.exe (Steam's own app info says
//    "falloutlauncher.exe", and the same file with "-classic" for "Play Classic Version"). So the
//    original launcher is backed up as FalloutLauncher.original.exe and the game takes its place:
//    Play then starts Fallout 1 Realtime -- on a PC, and through Proton on a Steam Deck, like
//    OpenMW through Morrowind. The game ignores "-classic" (it only parses [section]key=value).
//  * ANY OTHER INSTALL (GOG, CD): fallout1-realtime.exe goes next to MASTER.DAT; start it directly.
//  * Either way data\aimmask.dat goes in, and Fallout1-Realtime.txt says what was installed.
//
// Uninstall puts the original launcher back and removes exactly those files. fallout.cfg and saved
// games are never touched. Steam's "Verify integrity of game files" also restores the original
// launcher, which is the way back if the backup is ever lost.
//
// Run with no arguments for the window. For scripts and tests:
//   Fallout1-Realtime-Setup.exe [--target <folder>] --install | --uninstall | --detect
// prints what it does to stdout (redirect it) and exits 0 on success, 1 on failure.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <cwchar>
#include <string>
#include <vector>

#include "game/version.h"

// The payload, packed by the generated setup_payload_<config>.rc (see CMakeLists.txt).
#define IDR_GAME_EXE 101
#define IDR_AIMMASK 102

static const wchar_t* kLauncher = L"FalloutLauncher.exe";
static const wchar_t* kLauncherBackup = L"FalloutLauncher.original.exe";
static const wchar_t* kPlainExe = L"fallout1-realtime.exe";
static const wchar_t* kManifest = L"Fallout1-Realtime.txt";

// Every build of the game carries its window title; Steam's own launcher does not. This is how a
// FalloutLauncher.exe is recognised as ours (the development build's "(dev)" title matches too).
static const char* kOurMarker = "Fallout 1 Realtime";

static const wchar_t* kVersion = L"" REALTIME_VERSION;

// --- output ---------------------------------------------------------------------------------------

static HWND g_log = NULL;
static bool g_cli = false;

static std::string to_utf8(const std::wstring& w)
{
    if (w.empty()) {
        return std::string();
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

static std::wstring from_utf8(const std::string& s)
{
    if (s.empty()) {
        return std::wstring();
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static void say(const std::wstring& line)
{
    if (g_log != NULL) {
        int end = GetWindowTextLengthW(g_log);
        SendMessageW(g_log, EM_SETSEL, end, end);
        std::wstring text = line + L"\r\n";
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
    }

    if (g_cli) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out != NULL && out != INVALID_HANDLE_VALUE) {
            std::string bytes = to_utf8(line) + "\r\n";
            DWORD written;
            WriteFile(out, bytes.data(), (DWORD)bytes.size(), &written, NULL);
        }
    }
}

static std::wstring error_text(DWORD code)
{
    wchar_t* buffer = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, code, MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT), (LPWSTR)&buffer, 0, NULL);
    std::wstring text = buffer != NULL ? buffer : L"";
    if (buffer != NULL) {
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.')) {
        text.pop_back();
    }
    return text + L" (" + std::to_wstring(code) + L")";
}

// The one failure a player can do something about gets its own advice.
static void say_failure(const std::wstring& what, DWORD code)
{
    say(L"FAILED: " + what + L": " + error_text(code));
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
        say(L"  The game is probably running. Close it and try again.");
    } else if (code == ERROR_ACCESS_DENIED) {
        say(L"  Windows refused write access to that folder. Close the game if it is running;");
        say(L"  otherwise right-click the installer and choose \"Run as administrator\".");
    }
}

// --- files ----------------------------------------------------------------------------------------

static std::wstring join(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty()) {
        return name;
    }
    wchar_t last = dir.back();
    return (last == L'\\' || last == L'/') ? dir + name : dir + L"\\" + name;
}

static bool is_file(const std::wstring& path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool is_dir(const std::wstring& path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool read_file(const std::wstring& path, std::string& out)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(file, &size) != 0 && size.QuadPart < 512LL * 1024 * 1024;
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(file, &out[0], (DWORD)out.size(), &got, NULL) != 0 && got == out.size());
    }
    CloseHandle(file);
    return ok;
}

// Written beside the target first and then moved over it, so a failure halfway (disk full, the game
// holding the file open) never leaves a truncated exe where Steam's Play button points.
static bool write_file(const std::wstring& path, const void* data, DWORD size, DWORD* error)
{
    std::wstring temp = path + L".new";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return false;
    }

    DWORD written = 0;
    bool ok = WriteFile(file, data, size, &written, NULL) != 0 && written == size;
    *error = ok ? 0 : GetLastError();
    CloseHandle(file);

    if (ok && !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        *error = GetLastError();
        ok = false;
    }
    if (!ok) {
        DeleteFileW(temp.c_str());
    }
    return ok;
}

static bool payload(int id, const void** data, DWORD* size)
{
    HRSRC info = FindResourceW(NULL, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA
    if (info == NULL) {
        return false;
    }
    HGLOBAL handle = LoadResource(NULL, info);
    if (handle == NULL) {
        return false;
    }
    *data = LockResource(handle);
    *size = SizeofResource(NULL, info);
    return *data != NULL && *size > 0;
}

// --- finding the Steam copy -----------------------------------------------------------------------

static std::wstring registry_string(HKEY root, const wchar_t* key, const wchar_t* value)
{
    wchar_t buffer[1024];
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, NULL, buffer, &size) != ERROR_SUCCESS) {
        return std::wstring();
    }
    std::wstring path = buffer;
    for (wchar_t& c : path) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    while (!path.empty() && path.back() == L'\\') {
        path.pop_back();
    }
    return path;
}

// Steam's .vdf/.acf text: every quoted string in order, with \\ and \" unescaped. Braces do not
// matter for the two lookups needed here ("path" values, the "installdir" value).
static std::vector<std::string> vdf_tokens(const std::string& text)
{
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] != '"') {
            i++;
            continue;
        }
        std::string token;
        i++;
        while (i < text.size() && text[i] != '"') {
            if (text[i] == '\\' && i + 1 < text.size()) {
                token += text[i + 1];
                i += 2;
                continue;
            }
            token += text[i++];
        }
        i++;
        tokens.push_back(token);
    }
    return tokens;
}

static bool same_path(const std::wstring& a, const std::wstring& b)
{
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

static bool find_steam_fallout(std::wstring& found)
{
    std::vector<std::wstring> roots;
    const std::wstring candidates[] = {
        registry_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"),
        registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"),
        registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"),
    };
    for (const std::wstring& candidate : candidates) {
        if (!candidate.empty()) {
            roots.push_back(candidate);
        }
    }

    std::vector<std::wstring> libraries;
    auto add_library = [&libraries](const std::wstring& path) {
        for (const std::wstring& known : libraries) {
            if (same_path(known, path)) {
                return;
            }
        }
        libraries.push_back(path);
    };

    for (const std::wstring& root : roots) {
        add_library(root);
        std::string text;
        if (read_file(join(root, L"steamapps\\libraryfolders.vdf"), text)) {
            std::vector<std::string> tokens = vdf_tokens(text);
            for (size_t i = 0; i + 1 < tokens.size(); i++) {
                if (_stricmp(tokens[i].c_str(), "path") == 0) {
                    add_library(from_utf8(tokens[i + 1]));
                }
            }
        }
    }

    for (const std::wstring& library : libraries) {
        std::string manifest;
        if (!read_file(join(library, L"steamapps\\appmanifest_38400.acf"), manifest)) {
            continue;
        }

        std::wstring installDir = L"Fallout";
        std::vector<std::string> tokens = vdf_tokens(manifest);
        for (size_t i = 0; i + 1 < tokens.size(); i++) {
            if (_stricmp(tokens[i].c_str(), "installdir") == 0) {
                installDir = from_utf8(tokens[i + 1]);
                break;
            }
        }

        std::wstring dir = join(join(library, L"steamapps\\common"), installDir);
        if (is_file(join(dir, L"MASTER.DAT"))) {
            found = dir;
            return true;
        }
    }
    return false;
}

// --- what is in a folder --------------------------------------------------------------------------

struct Target {
    bool valid = false; // MASTER.DAT + CRITTER.DAT: it is a Fallout 1 folder at all
    bool steam = false; // Steam's launcher is (or was, per our backup) there
    bool launcherOurs = false;
    bool haveBackup = false;
    bool installed = false;
    std::wstring installedVersion;
};

static bool file_is_ours(const std::wstring& path)
{
    std::string bytes;
    return read_file(path, bytes) && bytes.find(kOurMarker) != std::string::npos;
}

static Target inspect(const std::wstring& dir)
{
    Target t;
    if (dir.empty() || !is_dir(dir)) {
        return t;
    }

    t.valid = is_file(join(dir, L"MASTER.DAT")) && is_file(join(dir, L"CRITTER.DAT"));
    t.haveBackup = is_file(join(dir, kLauncherBackup));
    bool launcher = is_file(join(dir, kLauncher));
    t.steam = launcher || t.haveBackup;
    t.launcherOurs = launcher && file_is_ours(join(dir, kLauncher));

    std::string manifest;
    bool haveManifest = read_file(join(dir, kManifest), manifest);
    if (haveManifest) {
        size_t at = manifest.find("version=");
        if (at != std::string::npos) {
            size_t end = manifest.find_first_of("\r\n", at);
            t.installedVersion = from_utf8(manifest.substr(at + 8, end == std::string::npos ? std::string::npos : end - at - 8));
        }
    }

    t.installed = t.launcherOurs || is_file(join(dir, kPlainExe)) || haveManifest;
    return t;
}

// --- install / uninstall --------------------------------------------------------------------------

static std::wstring now_text()
{
    SYSTEMTIME time;
    GetLocalTime(&time);
    wchar_t buffer[64];
    swprintf(buffer, 64, L"%04d-%02d-%02d %02d:%02d", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute);
    return buffer;
}

static bool do_install(const std::wstring& dir)
{
    Target t = inspect(dir);
    if (!t.valid) {
        say(L"FAILED: " + dir + L" is not a Fallout 1 folder (MASTER.DAT and CRITTER.DAT are missing).");
        return false;
    }

    const void* exe;
    DWORD exeSize;
    const void* mask;
    DWORD maskSize;
    if (!payload(IDR_GAME_EXE, &exe, &exeSize) || !payload(IDR_AIMMASK, &mask, &maskSize)) {
        say(L"FAILED: this installer is damaged (its game files are missing). Download it again.");
        return false;
    }

    say(L"Installing Fallout 1 Realtime " + std::wstring(kVersion) + L" into " + dir);

    DWORD error = 0;
    std::wstring files;
    if (t.steam) {
        std::wstring launcher = join(dir, kLauncher);
        std::wstring backup = join(dir, kLauncherBackup);

        if (is_file(launcher) && !t.launcherOurs) {
            // The launcher there now is Steam's -- possibly a fresh one after "Verify integrity",
            // so it replaces any older backup.
            if (!CopyFileW(launcher.c_str(), backup.c_str(), FALSE)) {
                say_failure(L"backing up FalloutLauncher.exe", GetLastError());
                return false;
            }
            say(L"  Backed up Steam's FalloutLauncher.exe as FalloutLauncher.original.exe.");
        } else if (!t.haveBackup) {
            say(L"  Note: there is no backup of Steam's original launcher. To go back to the original game,");
            say(L"  use Steam: right-click Fallout > Properties > Installed Files > Verify integrity of game files.");
        }

        if (!write_file(launcher, exe, exeSize, &error)) {
            say_failure(L"writing FalloutLauncher.exe", error);
            return false;
        }
        say(t.launcherOurs ? L"  Updated the game (FalloutLauncher.exe)." : L"  Installed the game as FalloutLauncher.exe.");
        files += L"FalloutLauncher.exe (the game; Steam's original is FalloutLauncher.original.exe)\r\n";
    } else {
        if (!write_file(join(dir, kPlainExe), exe, exeSize, &error)) {
            say_failure(L"writing fallout1-realtime.exe", error);
            return false;
        }
        say(L"  Installed fallout1-realtime.exe.");
        files += L"fallout1-realtime.exe\r\n";
    }

    // Windows paths are case-insensitive, so this lands in Steam's "DATA" as well as a "data".
    std::wstring dataDir = join(dir, L"data");
    if (!is_dir(dataDir) && !CreateDirectoryW(dataDir.c_str(), NULL)) {
        say_failure(L"creating the data folder", GetLastError());
        return false;
    }
    if (!write_file(join(dataDir, L"aimmask.dat"), mask, maskSize, &error)) {
        say_failure(L"writing data\\aimmask.dat", error);
        return false;
    }
    say(L"  Installed data\\aimmask.dat.");
    files += L"data\\aimmask.dat\r\n";

    std::wstring note = L"Fallout 1 Realtime\r\n"
                        L"version=" + std::wstring(kVersion) + L"\r\n"
                        L"mode=" + std::wstring(t.steam ? L"steam" : L"plain") + L"\r\n"
                        L"installed=" + now_text() + L"\r\n"
                        L"\r\n"
                        L"Files installed by Fallout1-Realtime-Setup.exe:\r\n" + files +
                        L"Fallout1-Realtime.txt (this file)\r\n"
                        L"\r\n"
                        L"To uninstall, run Fallout1-Realtime-Setup.exe again and choose Uninstall.\r\n"
                        L"fallout.cfg and saved games are never touched.\r\n";
    std::string noteBytes = to_utf8(note);
    if (!write_file(join(dir, kManifest), noteBytes.data(), (DWORD)noteBytes.size(), &error)) {
        say_failure(L"writing Fallout1-Realtime.txt", error);
        return false;
    }

    if (t.steam) {
        say(L"Done. Press Play on Fallout in Steam to start Fallout 1 Realtime.");
    } else {
        say(L"Done. Start fallout1-realtime.exe in that folder.");
    }
    return true;
}

static bool delete_if_present(const std::wstring& path, const wchar_t* label)
{
    if (!is_file(path)) {
        return true;
    }
    if (!DeleteFileW(path.c_str())) {
        say_failure(std::wstring(L"removing ") + label, GetLastError());
        return false;
    }
    say(std::wstring(L"  Removed ") + label + L".");
    return true;
}

static bool do_uninstall(const std::wstring& dir)
{
    Target t = inspect(dir);
    if (!t.valid) {
        say(L"FAILED: " + dir + L" is not a Fallout 1 folder (MASTER.DAT and CRITTER.DAT are missing).");
        return false;
    }
    if (!t.installed && !t.haveBackup) {
        say(L"Fallout 1 Realtime is not installed in " + dir + L". Nothing to do.");
        return true;
    }

    say(L"Uninstalling Fallout 1 Realtime from " + dir);
    bool ok = true;
    std::wstring launcher = join(dir, kLauncher);
    std::wstring backup = join(dir, kLauncherBackup);

    if (t.launcherOurs) {
        if (t.haveBackup) {
            if (!MoveFileExW(backup.c_str(), launcher.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                say_failure(L"restoring Steam's FalloutLauncher.exe", GetLastError());
                return false;
            }
            say(L"  Restored Steam's original FalloutLauncher.exe.");
        } else {
            say(L"  The backup of Steam's launcher is missing, so Play still starts Fallout 1 Realtime.");
            say(L"  To get the original back: right-click Fallout in Steam > Properties > Installed Files >");
            say(L"  Verify integrity of game files.");
        }
    } else if (t.haveBackup) {
        // Steam already put its launcher back ("Verify integrity"); the backup is a leftover.
        ok = delete_if_present(backup, L"the old launcher backup") && ok;
    }

    ok = delete_if_present(join(dir, kPlainExe), L"fallout1-realtime.exe") && ok;
    ok = delete_if_present(join(join(dir, L"data"), L"aimmask.dat"), L"data\\aimmask.dat") && ok;
    ok = delete_if_present(join(dir, kManifest), L"Fallout1-Realtime.txt") && ok;

    say(ok ? L"Done. fallout.cfg and saved games were left as they are." : L"Finished with errors (see above).");
    return ok;
}

// --- the window -----------------------------------------------------------------------------------

enum {
    ID_PATH = 1001,
    ID_BROWSE,
    ID_STATUS,
    ID_INSTALL,
    ID_UNINSTALL,
    ID_CLOSE,
    ID_LOG,
};

static HWND g_path = NULL;
static HWND g_status = NULL;
static HWND g_install = NULL;
static HWND g_uninstall = NULL;
static HFONT g_font = NULL;
static HFONT g_bold = NULL;
static bool g_detectedSteam = false;

static std::wstring window_text(HWND window)
{
    int length = GetWindowTextLengthW(window);
    std::wstring text(length, L'\0');
    if (length > 0) {
        GetWindowTextW(window, &text[0], length + 1);
    }
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\\')) {
        text.pop_back();
    }
    while (!text.empty() && text.front() == L' ') {
        text.erase(text.begin());
    }
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"') {
        text = text.substr(1, text.size() - 2);
    }
    return text;
}

static void refresh_status()
{
    std::wstring dir = window_text(g_path);
    Target t = inspect(dir);

    std::wstring status;
    if (dir.empty()) {
        status = L"Fallout was not found automatically. Choose the folder that contains MASTER.DAT.";
    } else if (!t.valid) {
        status = L"This is not a Fallout 1 folder: MASTER.DAT and CRITTER.DAT are missing.";
    } else {
        if (t.steam) {
            status = t.launcherOurs ? L"Steam version. Play in Steam starts Fallout 1 Realtime." : L"Steam version. After installing, Play in Steam starts Fallout 1 Realtime.";
        } else {
            status = L"Not the Steam version: you will start fallout1-realtime.exe from this folder.";
        }
        if (t.installed) {
            status += L"\r\nInstalled";
            if (!t.installedVersion.empty()) {
                status += L": version " + t.installedVersion;
            }
            status += L". This installer has version " + std::wstring(kVersion) + L".";
        } else {
            status += L"\r\nNot installed yet.";
        }
    }
    SetWindowTextW(g_status, status.c_str());

    SetWindowTextW(g_install, t.installed ? L"Update" : L"Install");
    EnableWindow(g_install, t.valid);
    EnableWindow(g_uninstall, t.valid && (t.installed || t.haveBackup));
}

static void browse(HWND owner)
{
    IFileOpenDialog* dialog = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Choose your Fallout 1 folder (the one with MASTER.DAT)");

    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = NULL;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            wchar_t* path = NULL;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                SetWindowTextW(g_path, path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
}

static int g_dpi = 96;

static int px(int value)
{
    return MulDiv(value, g_dpi, 96);
}

static HWND add_control(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font)
{
    HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), parent, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
    return control;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        std::wstring title = L"Fallout 1 Realtime " + std::wstring(kVersion);
        add_control(window, L"STATIC", title.c_str(), 0, 16, 14, 528, 22, 0, g_bold);
        add_control(window, L"STATIC",
            L"Installs into your Fallout 1. With the Steam version, the Play button in Steam then starts "
            L"Fallout 1 Realtime, on a PC and on a Steam Deck. Your fallout.cfg and saved games are not touched.",
            0, 16, 40, 528, 34, 0, g_font);
        add_control(window, L"STATIC", L"Fallout folder:", 0, 16, 84, 528, 18, 0, g_font);
        g_path = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            px(16), px(103), px(426), px(24), window, (HMENU)(INT_PTR)ID_PATH, NULL, NULL);
        SendMessageW(g_path, WM_SETFONT, (WPARAM)g_font, TRUE);
        add_control(window, L"BUTTON", L"Browse...", WS_TABSTOP, 450, 102, 94, 26, ID_BROWSE, g_font);
        g_status = add_control(window, L"STATIC", L"", 0, 16, 136, 528, 34, ID_STATUS, g_font);
        g_install = add_control(window, L"BUTTON", L"Install", WS_TABSTOP | BS_DEFPUSHBUTTON, 16, 178, 120, 30, ID_INSTALL, g_font);
        g_uninstall = add_control(window, L"BUTTON", L"Uninstall", WS_TABSTOP, 144, 178, 120, 30, ID_UNINSTALL, g_font);
        add_control(window, L"BUTTON", L"Close", WS_TABSTOP, 424, 178, 120, 30, ID_CLOSE, g_font);
        g_log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            px(16), px(218), px(528), px(166), window, (HMENU)(INT_PTR)ID_LOG, NULL, NULL);
        SendMessageW(g_log, WM_SETFONT, (WPARAM)g_font, TRUE);

        std::wstring found;
        g_detectedSteam = find_steam_fallout(found);
        if (g_detectedSteam) {
            SetWindowTextW(g_path, found.c_str());
            say(L"Found the Steam version of Fallout in " + found);
        } else {
            say(L"The Steam version of Fallout was not found. Use Browse... to choose your Fallout 1 folder.");
        }
        refresh_status();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_PATH:
            if (HIWORD(wParam) == EN_CHANGE) {
                refresh_status();
            }
            return 0;
        case ID_BROWSE:
            browse(window);
            return 0;
        case ID_INSTALL:
            say(L"");
            do_install(window_text(g_path));
            refresh_status();
            return 0;
        case ID_UNINSTALL:
            say(L"");
            do_uninstall(window_text(g_path));
            refresh_status();
            return 0;
        case ID_CLOSE:
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        // Static text on the window's own background, including the read-only log.
        HDC dc = (HDC)wParam;
        SetBkColor(dc, GetSysColor((HWND)lParam == g_log ? COLOR_WINDOW : COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush((HWND)lParam == g_log ? COLOR_WINDOW : COLOR_BTNFACE);
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

static int run_window(HINSTANCE instance)
{
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&controls);

    HDC screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);

    NONCLIENTMETRICSW metrics = { sizeof(metrics) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    g_font = CreateFontIndirectW(&metrics.lfMessageFont);
    LOGFONTW bold = metrics.lfMessageFont;
    bold.lfWeight = FW_BOLD;
    bold.lfHeight = MulDiv(bold.lfHeight, 4, 3);
    g_bold = CreateFontIndirectW(&bold);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"Fallout1RealtimeSetup";
    RegisterClassExW(&wc);

    RECT rect = { 0, 0, px(560), px(400) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rect, style, FALSE);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"Fallout 1 Realtime Setup", style,
        (GetSystemMetrics(SM_CXSCREEN) - width) / 2, (GetSystemMetrics(SM_CYSCREEN) - height) / 2, width, height,
        NULL, NULL, instance, NULL);
    ShowWindow(window, SW_SHOWNORMAL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}

// --- entry ----------------------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::wstring target;
    std::wstring action;
    for (int i = 1; i < argc; i++) {
        std::wstring arg = argv[i];
        if (arg == L"--target" && i + 1 < argc) {
            target = argv[++i];
        } else if (arg == L"--install" || arg == L"--uninstall" || arg == L"--detect") {
            action = arg;
        }
    }
    LocalFree(argv);

    if (action.empty()) {
        return run_window(instance);
    }

    g_cli = true;
    if (target.empty() && !find_steam_fallout(target)) {
        say(L"The Steam version of Fallout was not found. Pass --target <folder>.");
        return 1;
    }
    while (!target.empty() && target.back() == L'\\') {
        target.pop_back();
    }

    if (action == L"--detect") {
        Target t = inspect(target);
        say(L"folder=" + target);
        say(std::wstring(L"valid=") + (t.valid ? L"1" : L"0") + L" steam=" + (t.steam ? L"1" : L"0")
            + L" installed=" + (t.installed ? L"1" : L"0") + L" launcherOurs=" + (t.launcherOurs ? L"1" : L"0")
            + L" backup=" + (t.haveBackup ? L"1" : L"0") + L" version=" + t.installedVersion);
        return t.valid ? 0 : 1;
    }
    bool ok = action == L"--install" ? do_install(target) : do_uninstall(target);
    return ok ? 0 : 1;
}
