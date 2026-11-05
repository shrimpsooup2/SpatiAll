#include "dsp/Resampler.h"

#include <samplerate.h>

#include <cmath>

Resampler::~Resampler() {
    if (state_) src_delete(state_);
}

bool Resampler::init(int channels, double ratio, Quality q) {
    if (state_) src_delete(state_);
    state_ = nullptr;
    channels_ = channels;
    ratio_ = ratio;
    // Exact 1:1 with no drift control needed: skip the converter entirely.
    if (std::abs(ratio - 1.0) < 1e-12) return true;
    const int type = q == Quality::Fast ? SRC_SINC_FASTEST : q == Quality::Medium ? SRC_SINC_MEDIUM_QUALITY
                                                                                  : SRC_SINC_BEST_QUALITY;
    int err = 0;
    state_ = src_new(type, channels, &err);
    return state_ != nullptr;
}

void Resampler::reset() {
    if (state_) src_reset(state_);
}

void Resampler::process(const float* in, size_t frames, std::vector<float>& out) {
    if (!state_) {
        out.insert(out.end(), in, in + frames * channels_);
        return;
    }
    size_t consumed = 0;
    while (consumed < frames) {
        const size_t remaining = frames - consumed;
        const size_t capOut = static_cast<size_t>(std::ceil(remaining * ratio_)) + 64;
        const size_t base = out.size();
        out.resize(base + capOut * channels_);
        SRC_DATA d{};
        d.data_in = in + consumed * channels_;
        d.input_frames = static_cast<long>(remaining);
        d.data_out = out.data() + base;
        d.output_frames = static_cast<long>(capOut);
        d.src_ratio = ratio_;
        d.end_of_input = 0;
        if (src_process(state_, &d) != 0) {
            out.resize(base);
            return;
        }
        out.resize(base + static_cast<size_t>(d.output_frames_gen) * channels_);
        consumed += static_cast<size_t>(d.input_frames_used);
        if (d.input_frames_used == 0 && d.output_frames_gen == 0) break;
    }
}
