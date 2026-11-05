#pragma once
// Stereo -> centre + left/right residual, per frequency bin (passive coherence-based upmix).
//
// Rendering a stereo stem as two virtual sources sends anything centred to each ear twice with different
// delays (measured 3-5 dB comb ripple). Extracting the coherent, equal-level part into one centre source
// removes that, and leaves genuinely wide content in the residuals.
// STFT 1024 / hop 512 with sqrt-Hann analysis+synthesis; latency exactly one block (512 samples).

#include <vector>

class StereoUpmix {
public:
    static constexpr int kBlock = 512;

    void init();
    void reset();
    // Consumes kBlock samples of L/R, writes kBlock samples of centre/left/right (delayed by kBlock).
    void process(const float* inL, const float* inR, float* outC, float* outL, float* outR);

private:
    static constexpr int kN = 2 * kBlock;
    std::vector<float> window_, histL_, histR_, fl_, fr_, fc_, olaC_, olaL_, olaR_;
    std::vector<float> pll_, prr_, plrRe_, plrIm_;  // smoothed auto/cross spectra per bin
};
