#pragma once
// Single-producer / single-consumer ring of interleaved float frames.
// Producer and consumer may be different threads; no locks, no allocation after init.

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

class SpscRing {
public:
    SpscRing() = default;
    SpscRing(size_t capacityFrames, int channels) { init(capacityFrames, channels); }

    // Not thread-safe; call before producer/consumer start (or while both are stopped).
    void init(size_t capacityFrames, int channels) {
        channels_ = channels;
        cap_ = capacityFrames + 1;  // one slot kept empty to distinguish full from empty
        buf_.assign(cap_ * static_cast<size_t>(channels), 0.0f);
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    int channels() const { return channels_; }
    size_t capacity() const { return cap_ ? cap_ - 1 : 0; }

    size_t readable() const {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return h >= t ? h - t : h + cap_ - t;
    }
    size_t writable() const { return capacity() - readable(); }

    // Writes up to `frames`; returns frames written.
    size_t write(const float* src, size_t frames) {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_acquire);
        const size_t free = (t > h ? t - h : t + cap_ - h) - 1;
        const size_t n = frames < free ? frames : free;
        copyIn(h, src, n);
        head_.store((h + n) % cap_, std::memory_order_release);
        return n;
    }

    // Writes `frames` of silence; returns frames written.
    size_t writeSilence(size_t frames) {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_acquire);
        const size_t free = (t > h ? t - h : t + cap_ - h) - 1;
        const size_t n = frames < free ? frames : free;
        const size_t first = n < cap_ - h ? n : cap_ - h;
        std::memset(&buf_[h * channels_], 0, first * channels_ * sizeof(float));
        if (n > first) std::memset(&buf_[0], 0, (n - first) * channels_ * sizeof(float));
        head_.store((h + n) % cap_, std::memory_order_release);
        return n;
    }

    // Reads up to `frames`; returns frames read. `dst` may be null to discard.
    size_t read(float* dst, size_t frames) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t avail = h >= t ? h - t : h + cap_ - t;
        const size_t n = frames < avail ? frames : avail;
        if (dst) copyOut(t, dst, n);
        tail_.store((t + n) % cap_, std::memory_order_release);
        return n;
    }

    // Copies frames without consuming them; returns frames copied.
    size_t peek(float* dst, size_t frames) const {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t avail = h >= t ? h - t : h + cap_ - t;
        const size_t n = frames < avail ? frames : avail;
        copyOut(t, dst, n);
        return n;
    }

private:
    void copyIn(size_t pos, const float* src, size_t n) {
        const size_t first = n < cap_ - pos ? n : cap_ - pos;
        std::memcpy(&buf_[pos * channels_], src, first * channels_ * sizeof(float));
        if (n > first)
            std::memcpy(&buf_[0], src + first * channels_, (n - first) * channels_ * sizeof(float));
    }
    void copyOut(size_t pos, float* dst, size_t n) const {
        const size_t first = n < cap_ - pos ? n : cap_ - pos;
        std::memcpy(dst, &buf_[pos * channels_], first * channels_ * sizeof(float));
        if (n > first)
            std::memcpy(dst + first * channels_, &buf_[0], (n - first) * channels_ * sizeof(float));
    }

    std::vector<float> buf_;
    size_t cap_ = 0;
    int channels_ = 0;
    // Head and tail on separate cache lines (the resulting padding is intentional).
#pragma warning(suppress : 4324)
    alignas(64) std::atomic<size_t> head_{0};
#pragma warning(suppress : 4324)
    alignas(64) std::atomic<size_t> tail_{0};
};
