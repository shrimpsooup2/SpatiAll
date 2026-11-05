#include "spatial/Binaural.h"

#include "util/Log.h"

#include <phonon.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr float kDegToRad = 3.14159265358979f / 180.0f;

void IPLCALL logCallback(IPLLogLevel level, const char* message) {
    if (level == IPL_LOGLEVEL_ERROR) logx::error(std::string("steam audio: ") + message);
    else if (level == IPL_LOGLEVEL_WARNING) logx::warn(std::string("steam audio: ") + message);
}
}  // namespace

Vec3 directionFromAngles(float azimuthDeg, float elevationDeg) {
    const float az = azimuthDeg * kDegToRad;
    const float el = elevationDeg * kDegToRad;
    return Vec3{std::sin(az) * std::cos(el), std::sin(el), -std::cos(az) * std::cos(el)};
}

Binaural::~Binaural() { shutdown(); }

bool Binaural::init(int sampleRate, int frameSize, int numSources, std::string* error) {
    shutdown();
    auto fail = [&](const char* what, IPLerror e) {
        if (error) *error = std::string(what) + " failed (" + std::to_string(static_cast<int>(e)) + ")";
        shutdown();
        return false;
    };

    IPLContextSettings cs{};
    cs.version = STEAMAUDIO_VERSION;
    cs.logCallback = logCallback;
    cs.simdLevel = IPL_SIMDLEVEL_AVX2;
    IPLerror e = iplContextCreate(&cs, &context_);
    if (e != IPL_STATUS_SUCCESS) return fail("iplContextCreate", e);

    IPLAudioSettings as{sampleRate, frameSize};
    IPLHRTFSettings hs{};
    hs.type = IPL_HRTFTYPE_DEFAULT;
    hs.volume = 1.0f;
    hs.normType = IPL_HRTFNORMTYPE_NONE;
    e = iplHRTFCreate(context_, &as, &hs, &hrtf_);
    if (e != IPL_STATUS_SUCCESS) return fail("iplHRTFCreate", e);

    IPLBinauralEffectSettings bs{hrtf_};
    effects_.assign(static_cast<size_t>(numSources), nullptr);
    for (auto& fx : effects_) {
        e = iplBinauralEffectCreate(context_, &as, &bs, &fx);
        if (e != IPL_STATUS_SUCCESS) return fail("iplBinauralEffectCreate", e);
    }
    frameSize_ = frameSize;
    scaled_.assign(frameSize, 0.0f);
    tmpL_.assign(frameSize, 0.0f);
    tmpR_.assign(frameSize, 0.0f);
    lastGain_.assign(effects_.size(), 0.0f);
    return true;
}

void Binaural::shutdown() {
    for (auto& fx : effects_)
        if (fx) iplBinauralEffectRelease(&fx);
    effects_.clear();
    if (hrtf_) iplHRTFRelease(&hrtf_);
    if (context_) iplContextRelease(&context_);
    hrtf_ = nullptr;
    context_ = nullptr;
}

void Binaural::reset() {
    for (auto& fx : effects_) iplBinauralEffectReset(fx);
    std::fill(lastGain_.begin(), lastGain_.end(), 0.0f);
}

void Binaural::process(const float* const* inputs, const SourcePose* poses, float* outStereo) {
    float* tmp[2] = {tmpL_.data(), tmpR_.data()};
    IPLAudioBuffer out{2, frameSize_, tmp};

    for (size_t i = 0; i < effects_.size(); ++i) {
        const float target = poses[i].gain;
        const float start = lastGain_[i];
        lastGain_[i] = target;
        if (start == 0.0f && target == 0.0f) continue;  // silent source: skip the convolution entirely

        // Per-block linear gain ramp avoids zipper noise when gains/distances move.
        const float step = (target - start) / static_cast<float>(frameSize_);
        const float* in = inputs[i];
        for (int n = 0; n < frameSize_; ++n) scaled_[n] = in[n] * (start + step * static_cast<float>(n));

        float* inCh[1] = {scaled_.data()};
        IPLAudioBuffer inBuf{1, frameSize_, inCh};
        IPLBinauralEffectParams p{};
        p.direction = IPLVector3{poses[i].direction.x, poses[i].direction.y, poses[i].direction.z};
        p.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
        p.spatialBlend = 1.0f;
        p.hrtf = hrtf_;
        p.peakDelays = nullptr;
        iplBinauralEffectApply(effects_[i], &p, &inBuf, &out);
        for (int n = 0; n < frameSize_; ++n) {
            outStereo[2 * n] += tmpL_[n];
            outStereo[2 * n + 1] += tmpR_[n];
        }
    }
}
