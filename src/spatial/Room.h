#pragma once
// Shared virtual room: binaural early reflections from fixed room directions + a diffuse FDN reverb tail.
// Sources feed one mono send; their direct sound carries the position, the room supplies externalization and,
// through the direct-to-room ratio, distance.

#include "spatial/Binaural.h"
#include "spatial/Rotation.h"

#include <array>
#include <string>
#include <vector>

class Room {
public:
    enum class Preset { Off, SmallRoom, Studio, LivingRoom, Hall };

    bool init(int sampleRate, int frameSize, std::string* error);
    void setPreset(Preset p);
    Preset preset() const { return preset_; }
    void reset();

    // send: frameSize mono samples. Adds frameSize interleaved stereo frames (scaled by wetGain) to outStereo.
    void process(const float* send, const HeadRotation& head, float wetGain, float* outStereo);

private:
    static constexpr int kTaps = 8;
    static constexpr int kLines = 8;

    struct Tap {
        Vec3 dir;          // world direction the reflection arrives from
        float baseMs;      // delay at size 1
        float gain;
    };

    int sr_ = 0, frame_ = 0;
    Preset preset_ = Preset::Off;
    Binaural reflections_;
    std::array<Tap, kTaps> taps_{};
    std::array<int, kTaps> tapDelay_{};
    std::array<float, kTaps> tapLp_{};      // one-pole lowpass state per tap (wall absorption)
    float tapLpCoef_ = 0.0f;

    std::vector<float> sendBuf_;            // circular history of the send
    size_t sendPos_ = 0;

    std::array<std::vector<float>, kLines> lines_;
    std::array<size_t, kLines> linePos_{};
    std::array<float, kLines> lineGain_{}, lineLp_{};
    float lineDamp_ = 0.0f, lateGain_ = 0.0f, erGain_ = 1.0f;
    int preDelay_ = 0;

    std::vector<std::vector<float>> tapBufs_;
    std::vector<const float*> tapPtrs_;
    std::array<SourcePose, kTaps> poses_{};
};
