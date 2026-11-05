#pragma once
// WASAPI shared-mode loopback capture and render streams, plus endpoint enumeration/classification.
// Both expose interleaved float stereo to the engine; device-format conversion happens here.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace audio {

struct DeviceInfo {
    std::wstring id;
    std::string name;
    bool isDefault = false;
};

// Active render endpoints (playback devices).
std::vector<DeviceInfo> listRenderDevices();
std::wstring defaultRenderDeviceId();
std::string deviceName(const std::wstring& id);

// What kind of headphones an endpoint is, so the app can adapt (tracking profile, warnings).
enum class Connection { Unknown, Wired, Usb, Bluetooth };
struct EndpointTraits {
    Connection connection = Connection::Unknown;
    bool handsFree = false;      // Bluetooth hands-free/headset profile: mono, low-rate, spatial cues lost
    bool headphones = false;     // form factor says headphones/headset
    uint32_t sampleRate = 0;     // mix format
    uint16_t channels = 0;
    std::string model;           // recognised model family ("AirPods Pro", "AirPods Max", ...) or empty
};
EndpointTraits endpointTraits(const std::wstring& id);

uint32_t windowsBuild();

// Device sample format as seen by the converters.
struct StreamFormat {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;     // container bits per sample
    uint16_t validBits = 0;
    bool isFloat = false;
    uint32_t channelMask = 0;
};

class CaptureStream {
public:
    // Interleaved stereo float frames at `sampleRate()`.
    using Sink = std::function<void(const float* stereo, size_t frames)>;

    struct Config {
        std::wstring deviceId;  // render endpoint to tap (loopback)
    };

    CaptureStream() = default;
    ~CaptureStream();
    CaptureStream(const CaptureStream&) = delete;
    CaptureStream& operator=(const CaptureStream&) = delete;

    bool start(const Config& cfg, Sink sink);
    void stop();
    bool running() const { return running_.load(); }
    bool failed() const { return failed_.load(); }
    const std::string& error() const { return error_; }
    uint32_t sampleRate() const { return format_.sampleRate; }

private:
    void threadMain();

    Config cfg_;
    Sink sink_;
    StreamFormat format_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::string error_;
    void* startedEvent_ = nullptr;
    bool startOk_ = false;
};

class RenderStream {
public:
    // Fill `frames` interleaved stereo floats. Called on the real-time render thread.
    using Source = std::function<void(float* stereo, size_t frames)>;

    RenderStream() = default;
    ~RenderStream();
    RenderStream(const RenderStream&) = delete;
    RenderStream& operator=(const RenderStream&) = delete;

    bool start(const std::wstring& deviceId, Source source);
    void stop();
    bool running() const { return running_.load(); }
    bool failed() const { return failed_.load(); }
    const std::string& error() const { return error_; }
    uint32_t sampleRate() const { return format_.sampleRate; }
    // Device-side buffering (stream latency + buffer), seconds.
    double latencySeconds() const { return latency_; }

private:
    void threadMain();

    std::wstring deviceId_;
    Source source_;
    StreamFormat format_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::string error_;
    double latency_ = 0.0;
    void* startedEvent_ = nullptr;
    bool startOk_ = false;
};

}  // namespace audio
