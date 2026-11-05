#pragma once
// Streaming interleaved-float resampler (libsamplerate) with smoothly adjustable ratio for clock-drift control.

#include <cstddef>
#include <vector>

struct SRC_STATE_tag;

class Resampler {
public:
    enum class Quality { Fast, Medium, Best };

    Resampler() = default;
    ~Resampler();
    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;

    // ratio = outputRate / inputRate.
    bool init(int channels, double ratio, Quality q = Quality::Medium);
    void reset();
    // Ratio changes are ramped across the next processed block by libsamplerate.
    void setRatio(double ratio) { ratio_ = ratio; }
    double ratio() const { return ratio_; }
    bool passthrough() const { return state_ == nullptr; }

    // Consumes all `frames` input frames and appends produced frames to `out`.
    void process(const float* in, size_t frames, std::vector<float>& out);

private:
    SRC_STATE_tag* state_ = nullptr;
    int channels_ = 0;
    double ratio_ = 1.0;
};
