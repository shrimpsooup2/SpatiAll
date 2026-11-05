#include "spatial/Upmix.h"

#include <pocketfft_hdronly.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr float kSmooth = 0.2f;  // per-frame spectral smoothing (~50 ms time constant at 11.6 ms hops)
constexpr float kEps = 1e-12f;
}

void StereoUpmix::init() {
    window_.resize(kN);
    for (int i = 0; i < kN; ++i)  // sqrt of periodic Hann: analysis * synthesis sums to 1 at 50% overlap
        window_[i] = std::sqrt(0.5f - 0.5f * std::cos(2.0f * 3.14159265358979f * static_cast<float>(i) / kN));
    for (auto* v : {&histL_, &histR_, &fl_, &fr_, &fc_, &olaC_, &olaL_, &olaR_}) v->assign(kN, 0.0f);
    for (auto* v : {&pll_, &prr_, &plrRe_, &plrIm_}) v->assign(kN / 2 + 1, 0.0f);
}

void StereoUpmix::reset() { init(); }

void StereoUpmix::process(const float* inL, const float* inR, float* outC, float* outL, float* outR) {
    static thread_local pocketfft::detail::pocketfft_r<float> plan(kN);

    std::memmove(histL_.data(), histL_.data() + kBlock, kBlock * sizeof(float));
    std::memmove(histR_.data(), histR_.data() + kBlock, kBlock * sizeof(float));
    std::memcpy(histL_.data() + kBlock, inL, kBlock * sizeof(float));
    std::memcpy(histR_.data() + kBlock, inR, kBlock * sizeof(float));
    for (int i = 0; i < kN; ++i) {
        fl_[i] = histL_[i] * window_[i];
        fr_[i] = histR_[i] * window_[i];
    }
    plan.exec(fl_.data(), 1.0f, true);  // halfcomplex: r0, r1, i1, ..., r(N/2)
    plan.exec(fr_.data(), 1.0f, true);

    // Bin k (1..N/2-1) lives at [2k-1] (re) and [2k] (im); DC at [0], Nyquist at [N-1] (real only).
    for (int k = 0; k <= kN / 2; ++k) {
        const int ir = k == 0 ? 0 : (k == kN / 2 ? kN - 1 : 2 * k - 1);
        const bool hasIm = k != 0 && k != kN / 2;
        const float lr = fl_[ir], li = hasIm ? fl_[ir + 1] : 0.0f;
        const float rr = fr_[ir], ri = hasIm ? fr_[ir + 1] : 0.0f;
        pll_[k] += kSmooth * (lr * lr + li * li - pll_[k]);
        prr_[k] += kSmooth * (rr * rr + ri * ri - prr_[k]);
        plrRe_[k] += kSmooth * (lr * rr + li * ri - plrRe_[k]);  // L * conj(R)
        plrIm_[k] += kSmooth * (li * rr - lr * ri - plrIm_[k]);
        // Panning similarity: 1 for coherent equal-level (centre) content, 0 for one-sided or uncorrelated.
        const float cross = std::sqrt(plrRe_[k] * plrRe_[k] + plrIm_[k] * plrIm_[k]);
        const float m = std::clamp(2.0f * cross / (pll_[k] + prr_[k] + kEps), 0.0f, 1.0f);
        const float cr = 0.5f * m * (lr + rr), ci = 0.5f * m * (li + ri);
        fc_[ir] = cr;
        fl_[ir] = lr - cr;
        fr_[ir] = rr - cr;
        if (hasIm) {
            fc_[ir + 1] = ci;
            fl_[ir + 1] = li - ci;
            fr_[ir + 1] = ri - ci;
        }
    }

    const float inv = 1.0f / kN;
    plan.exec(fc_.data(), inv, false);
    plan.exec(fl_.data(), inv, false);
    plan.exec(fr_.data(), inv, false);
    for (int i = 0; i < kN; ++i) {
        olaC_[i] += fc_[i] * window_[i];
        olaL_[i] += fl_[i] * window_[i];
        olaR_[i] += fr_[i] * window_[i];
    }
    std::memcpy(outC, olaC_.data(), kBlock * sizeof(float));
    std::memcpy(outL, olaL_.data(), kBlock * sizeof(float));
    std::memcpy(outR, olaR_.data(), kBlock * sizeof(float));
    for (auto* ola : {&olaC_, &olaL_, &olaR_}) {
        std::memmove(ola->data(), ola->data() + kBlock, kBlock * sizeof(float));
        std::fill(ola->begin() + kBlock, ola->end(), 0.0f);
    }
}
