#pragma once
// Is a video playing? Media keys/SMTC can't tell (Chromium reports every tab as "Music"), so this looks at what
// is actually producing sound: a known video player, or a browser whose visible window shows a video site.

#include "audio/Routing.h"

#include <string>
#include <vector>

namespace app {

// Comma-separated user list. "name.exe" = video player process; "!text" = never video (e.g. "!YouTube Music");
// anything else = text in a browser window title (the active tab), e.g. "- YouTube", "Netflix".
inline constexpr const char* kDefaultVideoMarkers =
    "vlc.exe, mpv.exe, mpc-hc64.exe, mpc-hc.exe, mpc-be64.exe, potplayermini64.exe, potplayer.exe, video.ui.exe, "
    "kodi.exe, plex.exe, plexhtpc.exe, stremio.exe, netflix.exe, disneyplus.exe, "
    "- YouTube, Netflix, Twitch, Prime Video, Disney+, Crunchyroll, Hulu, HBO Max, Vimeo, Kick, Dailymotion, "
    "!YouTube Music";

// Returns what was detected ("video: YouTube in chrome.exe", "video: vlc.exe") or empty. `sessions` are the
// current audio sessions (only active ones count). Cheap enough to call about once a second.
std::string detectVideo(const std::vector<audio::AppSession>& sessions, const std::string& markers);

}  // namespace app
