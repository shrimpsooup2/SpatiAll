#pragma once
// Audio routing tricks:
//  - system default render endpoint (undocumented IPolicyConfig)
//  - per-application default endpoint (undocumented Windows.Media.Internal.AudioPolicyConfig)
//  - audio session enumeration (for the app picker)
// Every change is journaled to disk so a crash can be undone on next launch.

#include <cstdint>
#include <string>
#include <vector>

namespace audio {

struct AppSession {
    uint32_t pid = 0;
    std::string exe;          // e.g. "chrome.exe"
    std::string displayName;  // session display name if set, else exe
    bool active = false;      // currently playing
};

// Unique processes that own render sessions (system sounds excluded).
std::vector<AppSession> listAppSessions();

bool setDefaultRenderEndpoint(const std::wstring& deviceId);  // all roles

bool perAppRoutingSupported();
// deviceId empty = reset the app to follow the system default.
bool setAppRenderEndpoint(uint32_t pid, const std::wstring& deviceId);
std::wstring getAppRenderEndpoint(uint32_t pid);

// Tracks every change made so it can be reverted (on toggle off, exit, or after a crash).
class RoutingJournal {
public:
    static RoutingJournal& instance();

    // Apply + journal.
    bool redirectSystemDefault(const std::wstring& toDeviceId);
    bool redirectApp(uint32_t pid, const std::wstring& toDeviceId);

    // Revert everything this journal recorded, newest first.
    void restoreAll();
    // Revert a single app's routing.
    void restoreApp(uint32_t pid);
    // Called at startup: reverts changes left behind by a crashed previous run.
    void recoverFromCrash();
    // Journal file name inside %LOCALAPPDATA%\Spatiall (default routing-journal.txt). Tools that run beside the
    // app use their own file so they never recover or overwrite the app's live routing state.
    void useFile(const std::wstring& fileName);

    bool systemRedirected() const { return !originalDefault_.empty(); }

private:
    RoutingJournal() = default;
    void save() const;

    std::wstring originalDefault_;
    struct AppEntry {
        uint32_t pid;
        std::wstring originalEndpoint;  // empty = followed system default
    };
    std::vector<AppEntry> apps_;
};

}  // namespace audio
