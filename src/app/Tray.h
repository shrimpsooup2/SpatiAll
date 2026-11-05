#pragma once
// Notification-area icon: state-coloured icon, tooltip, left click = show window, right click = context menu.

#include <windows.h>

#include <array>
#include <string>
#include <vector>

namespace app {

class Tray {
public:
    static constexpr UINT kCallbackMessage = WM_APP + 1;

    enum class State { Off, Starting, Spatial, Stereo, Error, Count };
    enum Command : UINT { CmdNone = 0, CmdToggleSpatial = 40001, CmdTogglePower, CmdShow, CmdExit, CmdRecenter };
    // Layouts submenu: command = kCmdLayoutBase + layout index.
    static constexpr UINT kCmdLayoutBase = 41000;
    static constexpr UINT kMaxMenuLayouts = 500;

    struct MenuState {
        bool spatialOn = false;
        bool powerOn = false;
        bool trackingOn = false;  // Recenter is greyed out otherwise
        std::vector<std::string> layouts;
        int activeLayout = -1;
    };

    Tray() = default;
    ~Tray();
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    bool create(HWND hwnd);
    void destroy();

    // Updates icon + tooltip (no-op when unchanged).
    void update(State state, const std::wstring& tooltip);

    // Explorer restarted: the icon must be added again.
    UINT taskbarCreatedMessage() const { return taskbarCreatedMsg_; }
    void readd();

    // Blocks in TrackPopupMenu; returns the chosen Command or kCmdLayoutBase + layout index (CmdNone if nothing).
    UINT showMenu(POINT anchor, const MenuState& state);

private:
    bool add();

    HWND hwnd_ = nullptr;
    UINT taskbarCreatedMsg_ = 0;
    bool added_ = false;
    State state_ = State::Off;
    std::wstring tooltip_ = L"Spatiall";
    std::array<HICON, static_cast<size_t>(State::Count)> icons_{};
};

}  // namespace app
