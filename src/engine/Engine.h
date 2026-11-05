#pragma once
// Live pipeline: capture -> (separation) -> binaural spatialization -> headphones.
//
// Threads: capture thread(s) push into input rings; the worker thread resamples to 44.1 kHz and runs the
// separator, pushing aligned [dry | stems] frames into the scene ring; the render thread spatializes those
// frames block by block and resamples to the headphone rate with clock-drift correction.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace track {
class HeadTracker;
}

namespace engine {

constexpr int kMaxStems = 6;
constexpr int kInternalRate = 44100;  // htdemucs rate; separation + spatialization run here

// Saved as integers: keep the order.
enum class Mode {
    Stems,            // ML separation; each stem placed in space (~1 s latency); hop adapts to GPU load
    VirtualSpeakers,  // no ML; stereo L/R rendered as two virtual speakers (~60 ms latency)
    Adaptive,         // Stems, switching live to VirtualSpeakers for fullscreen apps/games or a starved GPU
};

enum class CaptureMode {
    WholeSystem,   // system default output -> silent sink; tap the sink
    SelectedApps,  // only chosen apps -> silent sink; tap the sink
};

// Stereo content of a stem (or the whole mix in VirtualSpeakers mode) is split into a centre part rendered
// at azimuth and left/right residuals rendered at azimuth -/+ width/2.
struct StemParams {
    float azimuthDeg = 0.0f;    // 0 ahead, +90 right, 180 behind (room-fixed when head tracking)
    float elevationDeg = 0.0f;  // +90 above
    float distance = 1.0f;      // metres (0.3 .. 5); also sets the direct-to-room ratio
    float widthDeg = 30.0f;     // spread of the stem's left/right residuals
    float gainDb = 0.0f;
    bool mute = false;
    bool solo = false;
};

enum class RoomPreset { Off, SmallRoom, Studio, LivingRoom, Hall };

struct SceneParams {
    std::array<StemParams, kMaxStems> stems{};  // Stems mode, indexed like Status::stemNames
    StemParams speakers{0.0f, 0.0f, 1.5f, 60.0f, 0.0f, false, false};  // VirtualSpeakers mode
    float sceneYawDeg = 0.0f;  // rotates everything (positive = scene turns right)
    float masterGainDb = 0.0f;
    RoomPreset room = RoomPreset::Studio;  // reflections + reverb that place sounds outside the head
    float roomWetDb = -8.0f;               // room level (-30 .. 0 dB)
};

struct Config {
    std::wstring headphoneDeviceId;  // render endpoint we output to
    std::wstring sinkDeviceId;       // silent endpoint apps are routed into (WholeSystem / SelectedApps)
    CaptureMode capture = CaptureMode::WholeSystem;
    std::vector<uint32_t> appPids;   // SelectedApps
    Mode mode = Mode::Adaptive;
    std::wstring modelPath;          // .onnx; sidecar .json lists sources
    float lookaheadSec = 0.3f;       // future context given to the separator
    float hopSec = 0.3f;             // shortest separator step; grows automatically when the GPU is busy
    float maxHopSec = 0.8f;          // longest step the adaptive hop may use
    bool adaptiveGpuFallback = true; // Adaptive: virtual speakers while the GPU can't keep up
};

struct Status {
    bool running = false;
    bool enabled = true;           // spatial processing (false = plain stereo through the same path)
    std::string error;             // last fatal error, empty if none
    std::string stage;             // human-readable state ("Loading model", "Running", ...)
    double latencySec = 0.0;       // capture -> ear estimate
    double inferenceMs = 0.0;      // last separator run
    double gpuLoad = 0.0;          // inferenceMs / hop
    float inputPeak = 0.0f;
    std::array<float, kMaxStems> stemLevels{};  // RMS, linear
    uint64_t underruns = 0;
    double renderLoad = 0.0;       // audio-thread CPU time / real time (smoothed)
    double renderLoadPeak = 0.0;   // recent worst callback
    std::vector<std::string> stemNames;  // from model metadata
    uint32_t captureRate = 0;
    uint32_t renderRate = 0;
    std::string provider;          // "DirectML" / "CPU"
    bool headTracked = false;      // tracker connected and receiving
    float headYawDeg = 0.0f, headPitchDeg = 0.0f, headRollDeg = 0.0f;  // smoothed orientation in use
    Mode activeMode = Mode::Stems;  // what is rendering now (Stems or VirtualSpeakers)
    double hopSec = 0.0;            // current separator step
    std::string adaptiveReason;     // why Adaptive chose activeMode ("fullscreen app", "GPU busy", ...)
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Applies routing, starts streams. Blocks while loading the model (seconds on first run).
    bool start(const Config& cfg);
    // Stops streams and restores all routing / mutes.
    void stop();
    bool running() const;

    // Crossfades between spatial output and plain stereo (same latency, no glitch).
    void setEnabled(bool on);
    bool enabled() const;

    void setScene(const SceneParams& s);
    SceneParams scene() const;

    // Head tracker whose orientation counter-rotates the scene (nullptr = static listener). The tracker must
    // outlive its use here: call setHeadTracker(nullptr) before destroying it. Takes effect within one block.
    void setHeadTracker(const track::HeadTracker* tracker);

    // Adaptive mode: something in front needs low latency more than separation (a game, a video). `reason` is
    // shown in the status ("fullscreen app", "video: YouTube"). The app decides which situations qualify.
    void setLowLatencyHint(bool on, const std::string& reason = {});

    Status status() const;
    const Config& config() const { return cfg_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Config cfg_;
};

}  // namespace engine
