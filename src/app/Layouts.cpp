#include "app/Layouts.h"

#include "app/Ini.h"
#include "app/Settings.h"
#include "util/Log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace app {
namespace {

constexpr wchar_t kFileName[] = L"layouts.ini";
constexpr size_t kMaxCustom = 256;

float wrapDeg(float d) {
    d = std::fmod(d + 180.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d - 180.0f;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }
float lerpDeg(float a, float b, float t) { return wrapDeg(a + wrapDeg(b - a) * t); }

struct Placement {
    float az, dist, width;
};

Placement presetPlacement(Preset preset, const std::string& name, size_t i, size_t count) {
    const float t = count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.5f;
    switch (preset) {
        // Every stem gets its own centre so orbs never sit on top of each other.
        case Preset::Stage:
            if (name == "drums") return {0.0f, 2.6f, 40.0f};
            if (name == "bass") return {-25.0f, 1.8f, 20.0f};
            if (name == "other") return {25.0f, 2.0f, 80.0f};
            if (name == "vocals") return {0.0f, 1.0f, 15.0f};
            if (name == "guitar") return {-45.0f, 2.0f, 30.0f};
            if (name == "piano") return {45.0f, 2.0f, 30.0f};
            return {-60.0f + 120.0f * t, 2.0f, 30.0f};
        case Preset::Surround:
            if (name == "drums") return {180.0f, 2.6f, 60.0f};
            if (name == "bass") return {-30.0f, 1.8f, 30.0f};
            if (name == "other") return {180.0f, 1.5f, 140.0f};  // wraps behind: L at +110, R at -110
            if (name == "vocals") return {0.0f, 1.0f, 20.0f};
            if (name == "guitar") return {-90.0f, 2.0f, 30.0f};
            if (name == "piano") return {90.0f, 2.0f, 30.0f};
            return {wrapDeg(360.0f * static_cast<float>(i) / static_cast<float>(std::max<size_t>(count, 1))), 2.0f,
                    40.0f};
        case Preset::Wide:
            if (name == "drums") return {-20.0f, 2.8f, 120.0f};
            if (name == "bass") return {20.0f, 2.2f, 60.0f};
            if (name == "other") return {0.0f, 3.4f, 180.0f};
            if (name == "vocals") return {0.0f, 1.5f, 40.0f};
            if (name == "guitar") return {-70.0f, 2.5f, 40.0f};
            if (name == "piano") return {70.0f, 2.5f, 40.0f};
            return {-90.0f + 180.0f * t, 2.5f, 60.0f};
        case Preset::Video:
            if (name == "vocals") return {0.0f, 1.0f, 20.0f};
            if (name == "drums") return {150.0f, 2.5f, 40.0f};
            if (name == "bass") return {-150.0f, 2.5f, 40.0f};
            if (name == "other") return {180.0f, 3.2f, 160.0f};  // +-100
            if (name == "guitar") return {-110.0f, 2.5f, 30.0f};
            if (name == "piano") return {110.0f, 2.5f, 30.0f};
            return {180.0f, 2.5f, 100.0f};
    }
    return {0.0f, 1.0f, 30.0f};
}

// The low-latency path (virtual speakers, also Adaptive's choice for games/videos): centred content - dialogue,
// lead vocals - stays at the front; only the stereo sides spread by `width`, so wider = more immersive without
// moving voices.
Placement speakerPlacement(Preset preset) {
    switch (preset) {
        case Preset::Stage: return {0.0f, 2.0f, 90.0f};
        case Preset::Surround: return {0.0f, 1.5f, 220.0f};
        case Preset::Wide: return {0.0f, 2.0f, 160.0f};
        case Preset::Video: return {0.0f, 1.3f, 200.0f};
    }
    return {0.0f, 1.0f, 60.0f};
}

void place(engine::StemParams& p, const Placement& pl) {
    p.azimuthDeg = wrapDeg(pl.az);
    p.elevationDeg = 0.0f;
    p.distance = pl.dist;
    p.widthDeg = pl.width;
}

engine::StemParams lerpStem(const engine::StemParams& a, const engine::StemParams& b, float t) {
    engine::StemParams r = b;
    r.azimuthDeg = lerpDeg(a.azimuthDeg, b.azimuthDeg, t);
    r.elevationDeg = lerp(a.elevationDeg, b.elevationDeg, t);
    r.distance = lerp(a.distance, b.distance, t);
    r.widthDeg = lerp(a.widthDeg, b.widthDeg, t);
    r.gainDb = lerp(a.gainDb, b.gainDb, t);
    return r;
}

std::vector<Layout> builtIns() {
    std::vector<Layout> out(4);
    out[0].name = "Stage";
    out[0].preset = Preset::Stage;
    out[1].name = "Surround";
    out[1].preset = Preset::Surround;
    out[2].name = "Wide";
    out[2].preset = Preset::Wide;
    out[3].name = "Video: voices front, music around";
    out[3].preset = Preset::Video;
    return out;
}

std::wstring builtInSection(Preset p) { return L"Builtin." + std::to_wstring(static_cast<int>(p)); }
std::wstring customSection(size_t i) { return L"Layout." + std::to_wstring(i); }

}  // namespace

engine::SceneParams presetScene(Preset preset, const std::vector<std::string>& stemNames,
                                const engine::SceneParams& current) {
    engine::SceneParams s = current;
    const size_t n = std::min(stemNames.size(), s.stems.size());
    for (size_t i = 0; i < n; ++i) place(s.stems[i], presetPlacement(preset, stemNames[i], i, n));
    place(s.speakers, speakerPlacement(preset));
    s.sceneYawDeg = 0.0f;
    return s;
}

engine::SceneParams resetScene(const engine::SceneParams& current) {
    engine::SceneParams s;
    s.masterGainDb = current.masterGainDb;
    s.room = current.room;
    s.roomWetDb = current.roomWetDb;
    return s;
}

Layout captureLayout(std::string name, const engine::SceneParams& scene, const std::vector<std::string>& stemNames) {
    Layout l;
    l.name = std::move(name);
    const size_t n = std::min(stemNames.size(), scene.stems.size());
    for (size_t i = 0; i < n; ++i) l.stems.emplace_back(stemNames[i], scene.stems[i]);
    l.speakers = scene.speakers;
    l.sceneYawDeg = scene.sceneYawDeg;
    l.room = scene.room;
    l.roomWetDb = scene.roomWetDb;
    return l;
}

engine::SceneParams layoutScene(const Layout& layout, const engine::SceneParams& current,
                                const std::vector<std::string>& stemNames) {
    if (layout.preset) return presetScene(*layout.preset, stemNames, current);
    engine::SceneParams s = current;
    const size_t n = std::min(stemNames.size(), s.stems.size());
    for (size_t i = 0; i < n; ++i) {
        for (const auto& [name, params] : layout.stems) {
            if (name == stemNames[i]) s.stems[i] = params;
        }
    }
    s.speakers = layout.speakers;
    s.sceneYawDeg = layout.sceneYawDeg;
    s.room = layout.room;
    s.roomWetDb = layout.roomWetDb;
    return s;
}

engine::SceneParams lerpScene(const engine::SceneParams& from, const engine::SceneParams& to, float t) {
    engine::SceneParams s = to;
    for (size_t i = 0; i < s.stems.size(); ++i) s.stems[i] = lerpStem(from.stems[i], to.stems[i], t);
    s.speakers = lerpStem(from.speakers, to.speakers, t);
    s.sceneYawDeg = lerpDeg(from.sceneYawDeg, to.sceneYawDeg, t);
    // Off has no meaningful level to fade from, so a room switching on fades in from the bottom of the range.
    const float fromWet = from.room == engine::RoomPreset::Off ? -30.0f : from.roomWetDb;
    s.roomWetDb = lerp(fromWet, to.roomWetDb, t);
    return s;
}

void Layouts::load() {
    items = builtIns();
    const std::wstring dir = Settings::directory();
    if (dir.empty()) return;
    const std::wstring file = dir + L"\\" + kFileName;
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    for (Layout& l : items) {
        l.hotkey = hotkeyFromString(ini::readString(file, builtInSection(*l.preset).c_str(), L"Hotkey"));
    }
    for (size_t i = 0; i < kMaxCustom; ++i) {
        const std::wstring secName = customSection(i);
        const wchar_t* sec = secName.c_str();
        const std::string name = logx::narrow(ini::readString(file, sec, L"Name"));
        if (name.empty()) break;
        Layout l;
        l.name = name;
        l.hotkey = hotkeyFromString(ini::readString(file, sec, L"Hotkey"));
        l.room = static_cast<engine::RoomPreset>(ini::readInt(file, sec, L"Room", static_cast<int>(l.room), 0,
                                                              static_cast<int>(engine::RoomPreset::Hall)));
        l.roomWetDb = ini::readFloat(file, sec, L"RoomWet", l.roomWetDb, -30.0f, 0.0f);
        l.sceneYawDeg = ini::readFloat(file, sec, L"Yaw", 0.0f, -180.0f, 180.0f);
        ini::readStem(file, sec, L"Speakers.", l.speakers);
        const std::wstring stems = ini::readString(file, sec, L"Stems");
        size_t start = 0;
        while (start < stems.size()) {
            const size_t bar = std::min(stems.find(L'|', start), stems.size());
            if (bar > start) {
                const std::wstring stem = stems.substr(start, bar - start);
                engine::StemParams p;
                ini::readStem(file, sec, stem + L".", p);
                l.stems.emplace_back(logx::narrow(stem), p);
            }
            start = bar + 1;
        }
        if (find(l.name) < 0) items.push_back(std::move(l));
    }
}

void Layouts::save() const {
    const std::wstring dir = Settings::directory();
    if (dir.empty()) return;
    // Rewritten from scratch so deleted layouts don't leave stale sections behind.
    DeleteFileW((dir + L"\\" + kFileName).c_str());
    const std::wstring file = ini::prepareFile(kFileName);
    if (file.empty()) return;

    bool ok = true;
    size_t custom = 0;
    for (const Layout& l : items) {
        std::wstring sec;
        if (l.preset) {
            if (l.hotkey.empty()) continue;
            ini::put(sec, L"Hotkey", hotkeyToString(l.hotkey));
            ok = ini::writeSection(file, builtInSection(*l.preset), sec) && ok;
            continue;
        }
        ini::put(sec, L"Name", logx::widen(l.name));
        ini::put(sec, L"Hotkey", hotkeyToString(l.hotkey));
        ini::putInt(sec, L"Room", static_cast<int>(l.room));
        ini::putFloat(sec, L"RoomWet", l.roomWetDb);
        ini::putFloat(sec, L"Yaw", l.sceneYawDeg);
        ini::putStem(sec, L"Speakers.", l.speakers);
        std::wstring stems;
        for (const auto& [name, params] : l.stems) {
            const std::wstring stem = logx::widen(name);
            if (!stems.empty()) stems += L'|';
            stems += stem;
            ini::putStem(sec, stem + L".", params);
        }
        ini::put(sec, L"Stems", stems);
        ok = ini::writeSection(file, customSection(custom++), sec) && ok;
    }
    ini::flush(file);
    if (!ok) logx::warn("Could not write layouts: " + logx::hr(HRESULT_FROM_WIN32(GetLastError())));
}

int Layouts::find(const std::string& name) const {
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace app
