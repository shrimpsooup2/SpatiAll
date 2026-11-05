#pragma once
// Scene layouts: the built-in presets plus user snapshots, applied by stem name so a layout saved with the 6-stem
// model also works with the 4-stem one. Custom layouts persist in %LOCALAPPDATA%\Spatiall\layouts.ini.

#include "app/Hotkey.h"
#include "engine/Engine.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace app {

enum class Preset { Stage, Surround, Wide, Video };

struct Layout {
    std::string name;
    std::optional<Preset> preset;  // built-in: placement computed for whatever stems the model has
    Hotkey hotkey;
    // Custom layouts only:
    std::vector<std::pair<std::string, engine::StemParams>> stems;  // by stem name
    engine::StemParams speakers;
    float sceneYawDeg = 0.0f;
    engine::RoomPreset room = engine::RoomPreset::Studio;
    float roomWetDb = -8.0f;

    bool builtIn() const { return preset.has_value(); }
};

// `current` with the preset's placement (azimuth, elevation, distance, width) for every stem and the speakers,
// rotation back to front; gains, mutes and the room stay.
engine::SceneParams presetScene(Preset preset, const std::vector<std::string>& stemNames,
                                const engine::SceneParams& current);
// Everything centred, 1 m away, gains 0 dB; room and master gain stay.
engine::SceneParams resetScene(const engine::SceneParams& current);

Layout captureLayout(std::string name, const engine::SceneParams& scene, const std::vector<std::string>& stemNames);
// The scene `layout` describes for the current model; stems the layout does not know keep their current values.
engine::SceneParams layoutScene(const Layout& layout, const engine::SceneParams& current,
                                const std::vector<std::string>& stemNames);
// Transition step (t in 0..1): angles along the shortest arc, everything else linear; discrete fields
// (room preset, mute, solo) and the master gain take `to`'s values.
engine::SceneParams lerpScene(const engine::SceneParams& from, const engine::SceneParams& to, float t);

struct Layouts {
    std::vector<Layout> items;  // built-ins first, then custom layouts in creation order

    // Built-ins plus the stored custom layouts and hotkeys.
    void load();
    void save() const;
    int find(const std::string& name) const;  // -1 if absent
};

}  // namespace app
