#pragma once
// Native reader for headphones implementing the Android Head Tracker HID protocol (Sony WH/WF-1000XM5/XM6, ...):
// a Sensors-page (0x20) "Other: Custom" (0xE1) collection described as "#AndroidHeadTracker#<version>", whose
// input reports carry the head orientation as a rotation vector. Samples are converted to track::Orientation and
// handed to HeadTracker::publish on a dedicated reader thread.

#include "track/HeadTracker.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace track {

class HidHeadSensor {
public:
    struct Info {
        std::wstring path;  // HID interface path, passed to start()
        std::string name;   // product / Bluetooth device name
    };

    // All connected head-tracker HID collections.
    static std::vector<Info> enumerate();

    HidHeadSensor() = default;
    ~HidHeadSensor();
    HidHeadSensor(const HidHeadSensor&) = delete;
    HidHeadSensor& operator=(const HidHeadSensor&) = delete;

    // Opens the sensor, switches it to full power / all events at ~100 Hz and starts the reader thread, which
    // calls sink.publish() per sample. `sink` must outlive stop().
    bool start(const std::wstring& path, HeadTracker& sink, std::string* error);
    // Stops reading and returns the sensor to power-off / no events. Safe to call repeatedly.
    void stop();
    const std::string& name() const { return name_; }

private:
    // Rotation-vector input field (Custom Value 1), resolved from the report descriptor at start().
    struct VectorField {
        uint8_t reportId = 0;
        uint16_t linkCollection = 0;
        uint16_t bitSize = 0;
        uint16_t packedBytes = 0;
        bool isSigned = false;
        double scale = 1.0, offset = 0.0;  // radians = logical * scale + offset
    };

    void stopLocked();
    void release();
    void readerMain();
    bool configure(bool on, std::string* error);
    bool decodeRotation(uint8_t* report, uint32_t bytes, double rv[3]) const;

    void* handle_ = nullptr;      // HANDLE, overlapped
    void* stopEvent_ = nullptr;   // HANDLE, manual reset
    void* preparsed_ = nullptr;   // PHIDP_PREPARSED_DATA
    uint16_t inputBytes_ = 0;
    uint16_t featureBytes_ = 0;
    VectorField rotation_;
    HeadTracker* sink_ = nullptr;
    std::thread thread_;
    std::string name_;
    std::mutex mu_;  // serialises start/stop
};

}  // namespace track
