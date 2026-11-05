#pragma once
// Real-time wrapper around HtDemucs: keeps the newest window of input, runs the model every `hop` samples,
// and emits the `hop` samples that end `lookahead` samples before the newest input (so the model has some
// future context). Consecutive outputs are crossfaded over the overlap both windows estimated.
// Validated offline by tools/eval_streaming.py (same scheme).

#include "sep/HtDemucs.h"

#include <vector>

class StreamingSeparator {
public:
    // Output frame layout: [dryL, dryR, s0L, s0R, s1L, s1R, ...]; dry is the input delayed to match.
    bool init(HtDemucs* model, int hopSamples, int lookaheadSamples);

    int channels() const { return 2 + 2 * model_->numSources(); }
    int hop() const { return hop_; }
    int maxHop() const { return W_ / 2 - lookahead_; }
    int latencySamples() const { return hop_ + lookahead_; }  // algorithmic delay, excluding inference time
    // Takes effect at the next step; clamped to [1024, maxHop()]. Look-ahead stays fixed.
    void setHop(int hopSamples);

    // Interleaved stereo at 44.1 kHz.
    void push(const float* stereo, size_t frames);
    bool ready() const { return pendingFrames() >= static_cast<size_t>(hop_); }

    // Consumes `hop` input frames, appends `hop` output frames to `out`. Returns false if inference failed
    // (the hop is then emitted as dry audio in the "other"/first stem so sound never drops out).
    bool step(std::vector<float>& out);
    // Moves all pending input into the window without running the model, so the separator can take over
    // later (e.g. after a low-latency spell) with a full window of real context.
    void absorb();
    double lastInferenceMs() const { return lastMs_; }
    bool lastSkipped() const { return lastSkipped_; }  // window was silent; model not run

private:
    size_t pendingFrames() const { return pending_.size() / 2 - pendingRead_; }

    HtDemucs* model_ = nullptr;
    int W_ = 0, hop_ = 0, lookahead_ = 0, xfade_ = 0;
    std::vector<float> hist_;     // planar [2][W], newest sample last
    std::vector<float> stems_;    // planar [S][2][W]
    std::vector<float> prevTail_; // planar [S][2][xfade]: previous window's estimate of the next region's start
    bool havePrev_ = false;
    std::vector<float> pending_;  // interleaved input not yet shifted into hist_
    size_t pendingRead_ = 0;
    size_t silentRun_ = 0;        // consecutive near-silent input samples
    int fallbackStem_ = 0;
    double lastMs_ = 0.0;
    bool lastSkipped_ = false;
};
