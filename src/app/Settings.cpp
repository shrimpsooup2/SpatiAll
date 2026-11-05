#include "app/Settings.h"

#include "app/Ini.h"
#include "util/Log.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cwchar>

namespace app {
namespace {

constexpr wchar_t kFileName[] = L"settings.ini";

std::wstring stemSectionName(size_t i) { return L"Stem" + std::to_wstring(i); }

// Device ids are long and full of braces/dots; a stable hash keeps section names short and valid.
std::wstring profileSectionName(const std::wstring& deviceId) {
    uint32_t h = 2166136261u;  // FNV-1a 32 over the UTF-8 id
    for (const char ch : logx::narrow(deviceId)) {
        h ^= static_cast<uint8_t>(ch);
        h *= 16777619u;
    }
    wchar_t buf[32];
    swprintf_s(buf, L"Headphone.%08x", h);
    return buf;
}

bool writeProfile(const std::wstring& file, const std::wstring& deviceId, const TrackingProfile& p) {
    std::wstring sec;
    ini::putInt(sec, L"Source", static_cast<int>(p.source));
    ini::put(sec, L"SensorPath", p.sensorPath);
    ini::putInt(sec, L"Port", p.udpPort);
    ini::putInt(sec, L"InvertYaw", p.invertYaw ? 1 : 0);
    ini::putInt(sec, L"InvertPitch", p.invertPitch ? 1 : 0);
    ini::putInt(sec, L"InvertRoll", p.invertRoll ? 1 : 0);
    ini::putFloat(sec, L"PredictionMs", p.predictionMs);
    return ini::writeSection(file, profileSectionName(deviceId), sec);
}

}  // namespace

std::wstring Settings::directory() {
    static const std::wstring dir = [] {
        PWSTR base = nullptr;
        std::wstring out;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
            out = std::wstring(base) + L"\\Spatiall";
        }
        CoTaskMemFree(base);
        return out;
    }();
    return dir;
}

std::wstring Settings::filePath() {
    const std::wstring dir = directory();
    return dir.empty() ? std::wstring() : dir + L"\\" + kFileName;
}

bool Settings::load() {
    const std::wstring file = filePath();
    if (file.empty() || GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    const wchar_t* g = L"General";
    headphoneDeviceId = ini::readString(file, g, L"Headphones");
    sinkDeviceId = ini::readString(file, g, L"SilentDevice");
    capture = static_cast<engine::CaptureMode>(ini::readInt(file, g, L"Capture", static_cast<int>(capture), 0, 1));
    mode = static_cast<engine::Mode>(ini::readInt(file, g, L"Mode", static_cast<int>(mode), 0, 2));
    adaptiveFullscreen = ini::readBool(file, g, L"AdaptiveFullscreen", adaptiveFullscreen);
    adaptiveVideo = ini::readBool(file, g, L"AdaptiveVideo", adaptiveVideo);
    if (const std::wstring markers = ini::readString(file, g, L"VideoMarkers"); !markers.empty()) {
        videoMarkers = logx::narrow(markers);
    }
    adaptiveGpuFallback = ini::readBool(file, g, L"AdaptiveGpuFallback", adaptiveGpuFallback);
    modelFile = ini::readString(file, g, L"Model");
    lookaheadSec = ini::readFloat(file, g, L"Lookahead", lookaheadSec, 0.1f, 2.0f);
    hopSec = ini::readFloat(file, g, L"Hop", hopSec, 0.2f, 1.0f);
    enabled = ini::readBool(file, g, L"Enabled", enabled);
    autostartPower = ini::readBool(file, g, L"AutostartPower", autostartPower);
    activeLayout = logx::narrow(ini::readString(file, g, L"ActiveLayout"));
    // Absent = keep the default; "0,0" = the user cleared it.
    const std::wstring quick = ini::readString(file, g, L"QuickMenuHotkey");
    if (!quick.empty()) quickMenuHotkey = hotkeyFromString(quick);

    appExes.clear();
    const std::string apps = logx::narrow(ini::readString(file, g, L"Apps"));
    size_t start = 0;
    while (start <= apps.size()) {
        const size_t bar = std::min(apps.find('|', start), apps.size());
        if (bar > start) appExes.push_back(apps.substr(start, bar - start));
        start = bar + 1;
    }

    const wchar_t* sc = L"Scene";
    scene.sceneYawDeg = ini::readFloat(file, sc, L"Yaw", scene.sceneYawDeg, -180.0f, 180.0f);
    scene.masterGainDb = ini::readFloat(file, sc, L"Master", scene.masterGainDb, -24.0f, 12.0f);
    scene.room = static_cast<engine::RoomPreset>(ini::readInt(file, sc, L"Room", static_cast<int>(scene.room), 0,
                                                              static_cast<int>(engine::RoomPreset::Hall)));
    scene.roomWetDb = ini::readFloat(file, sc, L"RoomWet", scene.roomWetDb, -30.0f, 0.0f);
    ini::readStem(file, L"Speakers", L"", scene.speakers);
    for (size_t i = 0; i < scene.stems.size(); ++i) {
        ini::readStem(file, stemSectionName(i).c_str(), L"", scene.stems[i]);
    }
    return true;
}

void Settings::save() const {
    const std::wstring file = ini::prepareFile(kFileName);
    if (file.empty()) return;

    std::string apps;
    for (const auto& exe : appExes) {
        if (!apps.empty()) apps += '|';
        apps += exe;
    }

    std::wstring general;
    ini::put(general, L"Headphones", headphoneDeviceId);
    ini::put(general, L"SilentDevice", sinkDeviceId);
    ini::putInt(general, L"Capture", static_cast<int>(capture));
    ini::put(general, L"Apps", logx::widen(apps));
    ini::putInt(general, L"Mode", static_cast<int>(mode));
    ini::putInt(general, L"AdaptiveFullscreen", adaptiveFullscreen ? 1 : 0);
    ini::putInt(general, L"AdaptiveVideo", adaptiveVideo ? 1 : 0);
    ini::put(general, L"VideoMarkers", logx::widen(videoMarkers));
    ini::putInt(general, L"AdaptiveGpuFallback", adaptiveGpuFallback ? 1 : 0);
    ini::put(general, L"Model", modelFile);
    ini::putFloat(general, L"Lookahead", lookaheadSec);
    ini::putFloat(general, L"Hop", hopSec);
    ini::putInt(general, L"Enabled", enabled ? 1 : 0);
    ini::putInt(general, L"AutostartPower", autostartPower ? 1 : 0);
    ini::put(general, L"ActiveLayout", logx::widen(activeLayout));
    ini::put(general, L"QuickMenuHotkey", quickMenuHotkey.empty() ? L"0,0" : hotkeyToString(quickMenuHotkey));

    std::wstring sceneSec;
    ini::putFloat(sceneSec, L"Yaw", scene.sceneYawDeg);
    ini::putFloat(sceneSec, L"Master", scene.masterGainDb);
    ini::putInt(sceneSec, L"Room", static_cast<int>(scene.room));
    ini::putFloat(sceneSec, L"RoomWet", scene.roomWetDb);
    std::wstring speakers;
    ini::putStem(speakers, L"", scene.speakers);

    bool ok = ini::writeSection(file, L"General", general);
    ok = ini::writeSection(file, L"Scene", sceneSec) && ok;
    if (!trackingDeviceId.empty()) ok = writeProfile(file, trackingDeviceId, tracking) && ok;
    ok = ini::writeSection(file, L"Speakers", speakers) && ok;
    for (size_t i = 0; i < scene.stems.size(); ++i) {
        std::wstring stem;
        ini::putStem(stem, L"", scene.stems[i]);
        ok = ini::writeSection(file, stemSectionName(i), stem) && ok;
    }
    ini::flush(file);
    if (!ok) logx::warn("Could not write settings: " + logx::hr(HRESULT_FROM_WIN32(GetLastError())));
}

bool Settings::loadTracking(const std::wstring& deviceId) {
    const std::wstring file = filePath();
    if (deviceId.empty() || file.empty()) return false;
    const std::wstring secName = profileSectionName(deviceId);
    const wchar_t* sec = secName.c_str();
    if (ini::readString(file, sec, L"Source").empty()) return false;

    TrackingProfile p;
    p.source = static_cast<track::Source>(
        ini::readInt(file, sec, L"Source", 0, 0, static_cast<int>(track::Source::HeadphoneSensor)));
    p.sensorPath = ini::readString(file, sec, L"SensorPath");
    p.udpPort = static_cast<uint16_t>(ini::readInt(file, sec, L"Port", p.udpPort, 1, 65535));
    p.invertYaw = ini::readBool(file, sec, L"InvertYaw", p.invertYaw);
    p.invertPitch = ini::readBool(file, sec, L"InvertPitch", p.invertPitch);
    p.invertRoll = ini::readBool(file, sec, L"InvertRoll", p.invertRoll);
    p.predictionMs = ini::readFloat(file, sec, L"PredictionMs", p.predictionMs, 0.0f, 250.0f);
    tracking = std::move(p);
    trackingDeviceId = deviceId;
    return true;
}

void Settings::saveTracking() const {
    if (trackingDeviceId.empty()) return;
    const std::wstring file = ini::prepareFile(kFileName);
    if (file.empty()) return;
    const bool ok = writeProfile(file, trackingDeviceId, tracking);
    ini::flush(file);
    if (!ok) logx::warn("Could not write headphone profile: " + logx::hr(HRESULT_FROM_WIN32(GetLastError())));
}

}  // namespace app
