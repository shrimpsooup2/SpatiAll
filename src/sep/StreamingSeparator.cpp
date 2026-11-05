#include "sep/StreamingSeparator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {
constexpr float kSilence = 1e-5f;  // ~ -100 dBFS
}

bool StreamingSeparator::init(HtDemucs* model, int hopSamples, int lookaheadSamples) {
    model_ = model;
    W_ = model->segment();
    lookahead_ = std::clamp(lookaheadSamples, 0, W_ / 2 - 1024);
    hop_ = std::clamp(hopSamples, 1024, maxHop());
    xfade_ = std::min(lookahead_, 882);  // 20 ms; needs look-ahead >= crossfade to have an overlap
    const size_t S = static_cast<size_t>(model->numSources());
    hist_.assign(2ull * W_, 0.0f);
    stems_.assign(S * 2 * W_, 0.0f);
    prevTail_.assign(S * 2 * xfade_, 0.0f);
    havePrev_ = false;
    pending_.clear();
    pendingRead_ = 0;
    silentRun_ = static_cast<size_t>(W_);  // history starts silent
    const auto& names = model->sources();
    const auto it = std::find(names.begin(), names.end(), "other");
    fallbackStem_ = it == names.end() ? 0 : static_cast<int>(it - names.begin());
    return true;
}

void StreamingSeparator::push(const float* stereo, size_t frames) {
    // Compact consumed input occasionally instead of on every step.
    if (pendingRead_ > 0 && pendingRead_ * 2 >= pending_.size() / 2) {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(pendingRead_ * 2));
        pendingRead_ = 0;
    }
    pending_.insert(pending_.end(), stereo, stereo + frames * 2);
}

void StreamingSeparator::setHop(int hopSamples) { hop_ = std::clamp(hopSamples, 1024, maxHop()); }

void StreamingSeparator::absorb() {
    const size_t n = pendingFrames();
    if (n == 0) return;
    const float* in = &pending_[pendingRead_ * 2];
    const size_t take = std::min(n, static_cast<size_t>(W_));
    const float* src = in + (n - take) * 2;  // only the newest W frames can matter
    bool loud = false;
    for (int c = 0; c < 2; ++c) {
        float* h = &hist_[static_cast<size_t>(c) * W_];
        std::memmove(h, h + take, (static_cast<size_t>(W_) - take) * sizeof(float));
        float* dst = h + (static_cast<size_t>(W_) - take);
        for (size_t i = 0; i < take; ++i) {
            dst[i] = src[2 * i + c];
            loud |= std::fabs(dst[i]) > kSilence;
        }
    }
    pendingRead_ += n;
    silentRun_ = loud ? 0 : silentRun_ + n;
    havePrev_ = false;  // the next step's output does not continue the last emitted one
}

bool StreamingSeparator::step(std::vector<float>& out) {
    const int S = model_->numSources();
    const int C = channels();

    // Shift the window left by hop and append hop new frames.
    const float* in = &pending_[pendingRead_ * 2];
    bool loud = false;
    for (int c = 0; c < 2; ++c) {
        float* h = &hist_[static_cast<size_t>(c) * W_];
        std::memmove(h, h + hop_, static_cast<size_t>(W_ - hop_) * sizeof(float));
        float* dst = h + (W_ - hop_);
        for (int i = 0; i < hop_; ++i) {
            dst[i] = in[2 * i + c];
            loud |= std::fabs(dst[i]) > kSilence;
        }
    }
    pendingRead_ += static_cast<size_t>(hop_);
    silentRun_ = loud ? 0 : silentRun_ + static_cast<size_t>(hop_);

    // Window entirely silent: the model would output silence; skip the GPU work.
    bool ok = true;
    lastSkipped_ = silentRun_ >= static_cast<size_t>(W_);
    if (lastSkipped_) {
        std::fill(stems_.begin(), stems_.end(), 0.0f);
        lastMs_ = 0.0;
    } else {
        const auto t0 = std::chrono::steady_clock::now();
        ok = model_->run(hist_.data(), stems_.data());
        lastMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!ok) {
            std::fill(stems_.begin(), stems_.end(), 0.0f);
            for (int c = 0; c < 2; ++c)
                std::memcpy(&stems_[(static_cast<size_t>(fallbackStem_) * 2 + c) * W_], &hist_[static_cast<size_t>(c) * W_],
                            static_cast<size_t>(W_) * sizeof(float));
        }
    }

    // Emit window region [W - lookahead - hop, W - lookahead).
    const int r0 = W_ - lookahead_ - hop_;
    const size_t base = out.size();
    out.resize(base + static_cast<size_t>(hop_) * C);
    float* o = &out[base];
    for (int i = 0; i < hop_; ++i) {
        float* f = o + static_cast<size_t>(i) * C;
        f[0] = hist_[r0 + i];
        f[1] = hist_[static_cast<size_t>(W_) + r0 + i];
        for (int s = 0; s < S; ++s)
            for (int c = 0; c < 2; ++c) f[2 + 2 * s + c] = stems_[(static_cast<size_t>(s) * 2 + c) * W_ + r0 + i];
    }
    if (havePrev_ && xfade_ > 0) {
        for (int i = 0; i < xfade_; ++i) {
            const float a = static_cast<float>(i + 1) / static_cast<float>(xfade_ + 1);
            float* f = o + static_cast<size_t>(i) * C;
            for (int s = 0; s < S; ++s)
                for (int c = 0; c < 2; ++c) {
                    const float prev = prevTail_[(static_cast<size_t>(s) * 2 + c) * xfade_ + i];
                    f[2 + 2 * s + c] = prev + a * (f[2 + 2 * s + c] - prev);
                }
        }
    }
    // Keep this window's estimate of the samples right after the region for the next crossfade.
    for (int s = 0; s < S; ++s)
        for (int c = 0; c < 2; ++c)
            std::memcpy(&prevTail_[(static_cast<size_t>(s) * 2 + c) * xfade_],
                        &stems_[(static_cast<size_t>(s) * 2 + c) * W_ + W_ - lookahead_], xfade_ * sizeof(float));
    havePrev_ = true;
    return ok;
}
