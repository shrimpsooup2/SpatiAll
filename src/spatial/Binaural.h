#pragma once
// Binaural rendering of a set of mono point sources with Steam Audio HRTFs.

#include <string>
#include <vector>

struct _IPLContext_t;
struct _IPLHRTF_t;
struct _IPLBinauralEffect_t;

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = -1.0f;  // listener space: +x right, +y up, -z ahead (unit length)
};

// Unit vector for azimuth (0 ahead, +90 right) and elevation (+90 above), degrees.
Vec3 directionFromAngles(float azimuthDeg, float elevationDeg);

struct SourcePose {
    Vec3 direction;    // listener space, unit length
    float gain = 1.0f; // linear, applied before the HRTF (ramped per block)
};

class Binaural {
public:
    Binaural() = default;
    ~Binaural();
    Binaural(const Binaural&) = delete;
    Binaural& operator=(const Binaural&) = delete;

    // Expensive (loads HRTF); call off the audio thread.
    bool init(int sampleRate, int frameSize, int numSources, std::string* error);
    void shutdown();
    bool ready() const { return context_ != nullptr; }
    int frameSize() const { return frameSize_; }
    int numSources() const { return static_cast<int>(effects_.size()); }

    // inputs[i]: `frameSize` mono samples for source i. Adds `frameSize` interleaved stereo frames into `outStereo`.
    void process(const float* const* inputs, const SourcePose* poses, float* outStereo);
    void reset();

private:
    _IPLContext_t* context_ = nullptr;
    _IPLHRTF_t* hrtf_ = nullptr;
    std::vector<_IPLBinauralEffect_t*> effects_;
    int frameSize_ = 0;
    std::vector<float> tmpL_, tmpR_;
    std::vector<float> scaled_;
    std::vector<float> lastGain_;
};
