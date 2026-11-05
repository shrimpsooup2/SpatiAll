#pragma once
// The main window: a top bar, the Stage card and a tabbed side panel. Edits app::Settings in place and reports
// what the App must act on.

#include "app/Layouts.h"
#include "app/Settings.h"
#include "audio/Routing.h"
#include "audio/Wasapi.h"
#include "engine/Engine.h"
#include "track/HeadTracker.h"
#include "track/HidHeadSensor.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ui {

// Source names of the bundled models (htdemucs: 4 stems, htdemucs_6s: 6), used until the engine reports its own.
std::vector<std::string> defaultStemNames(const std::wstring& modelFile);
// `stemNames` is the full list: "other" means something different with and without guitar/piano stems.
const char* stemDisplayName(const std::string& stemName, const std::vector<std::string>& stemNames);

inline constexpr int kCaptureNone = -3;
inline constexpr int kCaptureQuickMenu = -2;  // hotkey capture targets: layout index, or the quick menu

// Everything the panel shows but does not own; refreshed by the App each frame.
struct View {
    engine::Status status;
    std::vector<std::string> stemNames;  // never empty
    bool powerWanted = false;
    bool busy = false;                   // engine start/stop in flight
    std::string error;                   // banner text; empty = no banner
    std::vector<audio::DeviceInfo> devices;
    std::vector<audio::AppSession> sessions;
    std::vector<std::wstring> models;    // file names in the models folder
    std::wstring modelsDir;
    bool hotkeySpatialOk = true;
    bool hotkeyPowerOk = true;
    bool hotkeyRecenterOk = true;
    bool perAppRouting = false;
    uint32_t windowsBuild = 0;
    float dpiScale = 1.0f;
    audio::EndpointTraits headphoneTraits;           // of Settings::headphoneDeviceId
    std::vector<track::HidHeadSensor::Info> sensors;  // headphones with a HID head tracker
    std::vector<std::string> localIPv4;              // where phone/OpenTrack relays should send UDP
    track::TrackerStatus tracker;
    bool headTracked = false;                        // head is valid
    track::Orientation head;                         // listener orientation in use (recentered)

    const app::Layouts* layouts = nullptr;
    int activeLayout = -1;
    std::vector<bool> layoutHotkeyOk;                // per layout: registered (or unset)
    bool quickMenuHotkeyOk = true;
    int capturingHotkey = kCaptureNone;              // waiting for a key combo for this target
    bool quickMenuAvailable = true;                  // overlay could be created
    bool fullscreenApp = false;                      // a game or video fills the screen
    std::string lowLatencyReason;                    // Adaptive: why low latency is wanted now (empty = not)
};

struct Actions {
    bool togglePower = false;
    bool toggleSpatial = false;
    bool retry = false;
    bool refreshApps = false;
    bool refreshDevices = false;
    bool openLogFolder = false;
    bool hide = false;
    bool exit = false;
    bool sceneChanged = false;       // push Settings::scene to the engine now
    bool configChanged = false;      // restart the engine if it is on
    bool settingsChanged = false;    // persist only
    bool headphonesChanged = false;  // silent device may need a new automatic pick; tracking profile switches
    bool trackingChanged = false;    // restart the head tracker with Settings::tracking
    bool recenter = false;

    int applyLayout = -1;            // animate to this layout
    bool resetScene = false;
    std::string saveLayoutAs;        // non-empty: new custom layout from the current scene
    int updateLayout = -1;           // overwrite this custom layout with the current scene
    int deleteLayout = -1;
    int renameLayout = -1;
    std::string renameTo;
    int captureHotkey = kCaptureNone;  // start capturing a combo for this target
    int clearHotkey = kCaptureNone;
    bool cancelCapture = false;
};

class Ui {
public:
    Actions draw(app::Settings& s, const View& v);

private:
    enum class Tab { Mix, Layouts, Room, Headphones, Sources, Advanced };

    void drawTopBar(app::Settings& s, const View& v, Actions& a);
    void drawStage(app::Settings& s, const View& v, Actions& a);
    void drawMix(app::Settings& s, const View& v, Actions& a);
    void drawLayouts(app::Settings& s, const View& v, Actions& a);
    void drawRoom(app::Settings& s, const View& v, Actions& a);
    void drawHeadphones(app::Settings& s, const View& v, Actions& a);
    void drawSources(app::Settings& s, const View& v, Actions& a);
    void drawAdvanced(app::Settings& s, const View& v, Actions& a);
    void hotkeyButton(const char* id, const app::Hotkey& hk, bool ok, int target, const View& v, Actions& a);

    Tab tab_ = Tab::Mix;
    int selected_ = 0;   // stem shown in the Mix tab
    int dragging_ = -1;  // orb index being dragged (stems: index, speakers: 0 = L, 1 = R)
    std::array<float, engine::kMaxStems> smoothedLevel_{};
    float smoothedInput_ = 0.0f;
    char newLayoutName_[64] = {};
    int renaming_ = -1;
    char renameBuf_[64] = {};
};

}  // namespace ui
