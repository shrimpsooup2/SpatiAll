#include "spatial/Room.h"

#include <algorithm>
#include <cmath>

namespace {

struct PresetParams {
    float size;      // scales reflection delays and FDN line lengths
    float rt60;      // seconds
    float damping;   // in-loop one-pole lowpass coefficient (higher = darker tail)
    float erGain;    // early reflections relative level
    float lateGain;  // late tail relative level (before RT normalisation)
};

PresetParams paramsFor(Room::Preset p) {
    switch (p) {
        case Room::Preset::SmallRoom: return {0.55f, 0.25f, 0.35f, 1.0f, 0.5f};
        case Room::Preset::Studio: return {0.75f, 0.35f, 0.30f, 1.0f, 0.55f};
        case Room::Preset::LivingRoom: return {1.0f, 0.55f, 0.35f, 0.9f, 0.6f};
        case Room::Preset::Hall: return {2.2f, 1.7f, 0.22f, 0.6f, 0.8f};
        case Room::Preset::Off: break;
    }
    return {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
}

// Mutually prime base delays (samples @ 44.1 kHz, size 1) for the feedback delay network.
constexpr int kLineBase[8] = {1031, 1327, 1523, 1801, 2063, 2357, 2633, 2903};
// Output sign patterns giving two decorrelated ear signals from the same lines.
constexpr float kOutL[8] = {1, -1, 1, -1, 1, -1, 1, -1};
constexpr float kOutR[8] = {1, 1, -1, -1, 1, 1, -1, -1};

// In-place normalised 8-point Walsh-Hadamard transform: a lossless, fully mixing feedback matrix.
inline void hadamard8(float* v) {
    for (int len = 1; len < 8; len <<= 1)
        for (int i = 0; i < 8; i += len << 1)
            for (int j = i; j < i + len; ++j) {
                const float a = v[j], b = v[j + len];
                v[j] = a + b;
                v[j + len] = a - b;
            }
    constexpr float n = 0.35355339f;  // 1/sqrt(8)
    for (int i = 0; i < 8; ++i) v[i] *= n;
}

}  // namespace

bool Room::init(int sampleRate, int frameSize, std::string* error) {
    sr_ = sampleRate;
    frame_ = frameSize;
    // World-fixed reflection directions: side walls, floor, ceiling, rear walls, back wall, front wall.
    taps_ = {{
        {directionFromAngles(-60.0f, 0.0f), 7.0f, 0.75f},
        {directionFromAngles(60.0f, 0.0f), 8.5f, 0.75f},
        {directionFromAngles(0.0f, -50.0f), 4.5f, 0.65f},
        {directionFromAngles(0.0f, 55.0f), 9.5f, 0.55f},
        {directionFromAngles(-130.0f, 5.0f), 13.0f, 0.55f},
        {directionFromAngles(130.0f, 5.0f), 14.5f, 0.55f},
        {directionFromAngles(180.0f, 0.0f), 18.0f, 0.45f},
        {directionFromAngles(-20.0f, 15.0f), 21.0f, 0.4f},
    }};
    if (!reflections_.init(sampleRate, frameSize, kTaps, error)) return false;
    tapBufs_.assign(kTaps, std::vector<float>(frameSize, 0.0f));
    tapPtrs_.resize(kTaps);
    for (int i = 0; i < kTaps; ++i) tapPtrs_[i] = tapBufs_[i].data();
    // Longest reflection (21 ms x 2.2) plus pre-delay must fit in the send history.
    sendBuf_.assign(static_cast<size_t>(0.2 * sampleRate) + frameSize, 0.0f);
    for (int i = 0; i < kLines; ++i)
        lines_[i].assign(static_cast<size_t>(kLineBase[i] * 2.5 * sampleRate / 44100.0) + 1, 0.0f);
    // Wall absorption: ~7 kHz one-pole lowpass on reflections.
    tapLpCoef_ = std::exp(-2.0f * 3.14159265f * 7000.0f / static_cast<float>(sampleRate));
    setPreset(preset_);
    return true;
}

void Room::setPreset(Preset p) {
    preset_ = p;
    const PresetParams pp = paramsFor(p);
    for (int i = 0; i < kTaps; ++i) tapDelay_[i] = static_cast<int>(taps_[i].baseMs * pp.size * 0.001f * sr_);
    preDelay_ = static_cast<int>(22.0f * pp.size * 0.001f * sr_);
    erGain_ = pp.erGain;
    lineDamp_ = pp.damping;
    // A longer tail carries more energy; normalise so presets differ in character, not loudness.
    lateGain_ = pp.rt60 > 0.0f ? pp.lateGain * std::sqrt(0.4f / pp.rt60) : 0.0f;
    for (int i = 0; i < kLines; ++i) {
        const float len = kLineBase[i] * pp.size * static_cast<float>(sr_) / 44100.0f;
        // Per-pass gain so each line decays 60 dB in rt60 seconds.
        lineGain_[i] = pp.rt60 > 0.0f ? std::pow(10.0f, -3.0f * len / (pp.rt60 * static_cast<float>(sr_))) : 0.0f;
    }
    reset();
}

void Room::reset() {
    std::fill(sendBuf_.begin(), sendBuf_.end(), 0.0f);
    for (auto& l : lines_) std::fill(l.begin(), l.end(), 0.0f);
    linePos_.fill(0);
    lineLp_.fill(0.0f);
    tapLp_.fill(0.0f);
    reflections_.reset();
}

void Room::process(const float* send, const HeadRotation& head, float wetGain, float* outStereo) {
    if (preset_ == Preset::Off || wetGain <= 0.0f) return;
    const PresetParams pp = paramsFor(preset_);
    const size_t n = sendBuf_.size();

    for (int i = 0; i < frame_; ++i) sendBuf_[(sendPos_ + i) % n] = send[i];

    // Early reflections: delayed, absorbed copies of the send arriving from fixed room directions.
    for (int t = 0; t < kTaps; ++t) {
        float* dst = tapBufs_[t].data();
        float lp = tapLp_[t];
        const float g = taps_[t].gain * erGain_;
        for (int i = 0; i < frame_; ++i) {
            const float x = sendBuf_[(sendPos_ + i + n - static_cast<size_t>(tapDelay_[t])) % n];
            lp = x + tapLpCoef_ * (lp - x);
            dst[i] = lp * g;
        }
        tapLp_[t] = lp;
        poses_[t] = SourcePose{head.toListener(taps_[t].dir), wetGain};
    }
    reflections_.process(tapPtrs_.data(), poses_.data(), outStereo);

    // Late reverb: 8-line FDN fed by the pre-delayed send; diffuse, so it bypasses the HRTF.
    const float late = lateGain_ * wetGain;
    float v[kLines];
    for (int i = 0; i < frame_; ++i) {
        const float in = sendBuf_[(sendPos_ + i + n - static_cast<size_t>(preDelay_)) % n];
        float outL = 0.0f, outR = 0.0f;
        for (int l = 0; l < kLines; ++l) {
            const size_t len = static_cast<size_t>(kLineBase[l] * pp.size * static_cast<float>(sr_) / 44100.0f);
            const float y = lines_[l][(linePos_[l] + lines_[l].size() - len) % lines_[l].size()];
            lineLp_[l] = y + lineDamp_ * (lineLp_[l] - y);  // frequency-dependent decay
            v[l] = lineLp_[l] * lineGain_[l];
            outL += kOutL[l] * y;
            outR += kOutR[l] * y;
        }
        hadamard8(v);
        for (int l = 0; l < kLines; ++l) {
            lines_[l][linePos_[l]] = v[l] + in * ((l & 1) ? -0.5f : 0.5f);
            linePos_[l] = (linePos_[l] + 1) % lines_[l].size();
        }
        outStereo[2 * i] += outL * late * 0.35f;
        outStereo[2 * i + 1] += outR * late * 0.35f;
    }
    sendPos_ = (sendPos_ + frame_) % n;
}
