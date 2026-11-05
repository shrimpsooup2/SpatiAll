#pragma once
// Listener head orientation from an external tracker, read without blocking by the render thread.
//
// Sources:
//  - UDP: OpenTrack "UDP over network" datagrams (48 bytes: 6 little-endian doubles x, y, z [cm], yaw, pitch,
//    roll [deg], yaw + = look left) and FreePIE UDP (50 bytes, radians; used by the AirPods Linux bridge
//    pods-head-tracker). Covers OpenTrack itself (webcam, phone IMU, ...) and AirPods relays/bridges.
//  - Headphone sensor: headphones exposing the Android Head Tracker HID protocol (e.g. Sony WH/WF-1000XM5/XM6),
//    read natively over HID (see HidHeadSensor).

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace track {

// Degrees. yaw + = head turned right, pitch + = looking up, roll + = right ear down.
struct Orientation {
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
};

enum class Source { None, OpenTrackUdp, HeadphoneSensor };

struct TrackerStatus {
    bool running = false;
    bool receiving = false;   // samples within the last second
    double rateHz = 0.0;      // recent sample rate
    Orientation raw;          // latest, before recentering
    std::string device;       // sensor name (HeadphoneSensor) or "udp:<port>"
    std::string error;
};

class HidHeadSensor;

class HeadTracker {
public:
    struct Config {
        Source source = Source::None;
        uint16_t udpPort = 4242;
        std::wstring sensorPath;    // HeadphoneSensor: HID device path; empty = first sensor found
        bool invertYaw = false, invertPitch = false, invertRoll = false;
        float predictionMs = 0.0f;  // extrapolate ahead to hide Bluetooth/relay latency (0 = off)
    };

    HeadTracker();
    ~HeadTracker();
    HeadTracker(const HeadTracker&) = delete;
    HeadTracker& operator=(const HeadTracker&) = delete;

    bool start(const Config& cfg);
    void stop();
    const Config& config() const { return cfg_; }

    // Orientation relative to the recentered front, with prediction applied; `valid` false when no recent
    // samples (render side then eases back to front). Lock-free; safe on the real-time render thread.
    bool current(Orientation& out) const;
    // Makes the current raw orientation the new front.
    void recenter();
    TrackerStatus status() const;

    // Called by sources (any thread): a new raw sample in this file's convention.
    void publish(const Orientation& raw);

private:
    void udpMain();

    Config cfg_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    uintptr_t socket_ = ~uintptr_t(0);
    std::unique_ptr<HidHeadSensor> hid_;

    // Latest sample + angular velocity (deg/s) for prediction. The render thread only try-locks and falls back
    // to its previous copy, so it never blocks on the network/HID thread.
    struct Sample {
        Orientation o;
        Orientation vel;
        int64_t qpc = 0;
    };
    mutable std::mutex sampleMu_;
    Sample sample_;
    mutable Sample renderCopy_;           // render-thread copies (only touched by current())
    mutable Orientation renderCenter_;
    Orientation center_;
    std::atomic<double> rateHz_{0.0};
    std::atomic<float> predictionMs_{0.0f};  // copy of cfg_.predictionMs readable from the render thread

    mutable std::mutex errMu_;
    std::string error_;
};

}  // namespace track
