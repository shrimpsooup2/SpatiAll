#pragma once
// User settings persisted to %LOCALAPPDATA%\Spatiall\settings.ini (UTF-16 ini via the Win32 profile API).

#include "app/Hotkey.h"
#include "app/VideoDetector.h"
#include "engine/Engine.h"
#include "track/HeadTracker.h"

#include <cstdint>
#include <string>
#include <vector>

namespace app {

// Head tracking settings, stored per headphone device in [Headphone.<FNV-1a 32 of the device id>].
struct TrackingProfile {
    track::Source source = track::Source::None;
    std::wstring sensorPath;  // HID device path when source == HeadphoneSensor
    uint16_t udpPort = 4242;
    bool invertYaw = false, invertPitch = false, invertRoll = false;
    float predictionMs = 0.0f;
};

struct Settings {
    std::wstring headphoneDeviceId;
    std::wstring sinkDeviceId;
    engine::CaptureMode capture = engine::CaptureMode::WholeSystem;
    std::vector<std::string> appExes;  // lower-case UTF-8 exe names; resolved to pids at each engine start
    engine::Mode mode = engine::Mode::Adaptive;
    std::wstring modelFile;            // file name inside <exe dir>\models
    float lookaheadSec = 0.3f;
    float hopSec = 0.3f;               // shortest separator step; the engine lengthens it when the GPU is busy
    bool adaptiveFullscreen = true;    // Adaptive: low latency while a fullscreen app or game is in front
    bool adaptiveVideo = true;         // Adaptive: low latency while a video is playing (see VideoDetector)
    std::string videoMarkers = kDefaultVideoMarkers;  // video players / sites, see VideoDetector.h
    bool adaptiveGpuFallback = true;   // Adaptive: virtual speakers while the GPU can't keep up
    engine::SceneParams scene;
    bool enabled = true;
    bool autostartPower = false;
    TrackingProfile tracking;          // profile of trackingDeviceId
    std::wstring trackingDeviceId;     // headphones `tracking` belongs to; empty = not loaded / no headphones
    std::string activeLayout;          // name of the last applied layout (UTF-8), empty = none
    Hotkey quickMenuHotkey{kModControl | kModAlt, 0x20 /* VK_SPACE */};

    static std::wstring directory();  // %LOCALAPPDATA%\Spatiall
    static std::wstring filePath();

    // Returns false when no settings file exists yet (first run); fields keep their defaults then.
    bool load();
    // Also writes the tracking profile of trackingDeviceId.
    void save() const;

    // Loads the stored profile of `deviceId` into tracking/trackingDeviceId; false (nothing changed) if none.
    bool loadTracking(const std::wstring& deviceId);
    // Writes only the tracking profile of trackingDeviceId (before switching to other headphones).
    void saveTracking() const;
};

}  // namespace app
