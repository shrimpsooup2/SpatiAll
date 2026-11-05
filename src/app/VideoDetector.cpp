#include "app/VideoDetector.h"

#include "util/Log.h"

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>

namespace app {
namespace {

constexpr const char* kBrowsers[] = {"chrome.exe", "msedge.exe", "firefox.exe", "brave.exe", "opera.exe",
                                     "vivaldi.exe", "arc.exe", "librewolf.exe", "waterfox.exe", "floorp.exe"};

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

struct Markers {
    std::vector<std::string> exes, titles, excludes;  // lower-case
};

Markers parse(const std::string& list) {
    Markers m;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = std::min(list.find(',', start), list.size());
        const std::string item = lower(trim(list.substr(start, comma - start)));
        start = comma + 1;
        if (item.empty()) continue;
        if (item[0] == '!') {
            if (item.size() > 1) m.excludes.push_back(trim(item.substr(1)));
        } else if (item.size() > 4 && item.ends_with(".exe")) {
            m.exes.push_back(item);
        } else {
            m.titles.push_back(item);
        }
    }
    return m;
}

std::string exeOf(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    std::string out;
    if (QueryFullProcessImageNameW(h, 0, path, &len)) {
        out = lower(logx::narrow(std::filesystem::path(std::wstring(path, len)).filename().wstring()));
    }
    CloseHandle(h);
    return out;
}

struct WindowScan {
    const Markers* markers;
    const std::vector<std::string>* browsers;  // browsers that are currently playing audio
    std::map<DWORD, std::string> exeCache;
    std::string found;
};

BOOL CALLBACK onWindow(HWND hwnd, LPARAM lp) {
    auto& scan = *reinterpret_cast<WindowScan*>(lp);
    // A video someone is watching is on screen: visible, not minimised, not on another virtual desktop.
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    wchar_t title[512];
    const int n = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    if (n <= 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    auto it = scan.exeCache.find(pid);
    if (it == scan.exeCache.end()) it = scan.exeCache.emplace(pid, exeOf(pid)).first;
    const std::string& exe = it->second;
    if (std::find(scan.browsers->begin(), scan.browsers->end(), exe) == scan.browsers->end()) return TRUE;

    const std::string t = lower(logx::narrow(std::wstring(title, static_cast<size_t>(n))));
    for (const std::string& ex : scan.markers->excludes)
        if (t.find(ex) != std::string::npos) return TRUE;
    for (const std::string& marker : scan.markers->titles) {
        if (t.find(marker) == std::string::npos) continue;
        std::string shown = marker;
        if (shown.starts_with("- ")) shown = shown.substr(2);
        // Show the site as written in the window title rather than lower-cased.
        const size_t at = t.find(shown);
        const std::string original = logx::narrow(std::wstring(title, static_cast<size_t>(n)));
        if (at != std::string::npos && at + shown.size() <= original.size()) shown = original.substr(at, shown.size());
        scan.found = "video: " + shown + " in " + exe;
        return FALSE;
    }
    return TRUE;
}

}  // namespace

std::string detectVideo(const std::vector<audio::AppSession>& sessions, const std::string& markerList) {
    const Markers markers = parse(markerList);
    std::vector<std::string> playingBrowsers;
    for (const audio::AppSession& s : sessions) {
        if (!s.active) continue;
        const std::string exe = lower(s.exe);
        if (std::find(markers.exes.begin(), markers.exes.end(), exe) != markers.exes.end()) return "video: " + exe;
        if (std::find(std::begin(kBrowsers), std::end(kBrowsers), exe) != std::end(kBrowsers) &&
            std::find(playingBrowsers.begin(), playingBrowsers.end(), exe) == playingBrowsers.end()) {
            playingBrowsers.push_back(exe);
        }
    }
    if (playingBrowsers.empty() || markers.titles.empty()) return {};
    WindowScan scan{&markers, &playingBrowsers, {}, {}};
    EnumWindows(&onWindow, reinterpret_cast<LPARAM>(&scan));
    return scan.found;
}

}  // namespace app
