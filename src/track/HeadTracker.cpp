#include "track/HeadTracker.h"

#include "track/HidHeadSensor.h"
#include "util/Log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace track {
namespace {

int64_t qpcNow() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

double qpcSeconds(int64_t ticks) {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    return static_cast<double>(ticks) / freq;
}

float wrap180(float a) { return std::remainder(a, 360.0f); }

constexpr double kStaleSec = 1.0;          // no sample for this long = tracker lost
constexpr double kMaxExtrapolateSec = 0.3; // cap prediction so a stale velocity can't fling the scene

}  // namespace

HeadTracker::HeadTracker() = default;

HeadTracker::~HeadTracker() { stop(); }

bool HeadTracker::start(const Config& cfg) {
    stop();
    cfg_ = cfg;
    predictionMs_ = cfg.predictionMs;
    {
        std::lock_guard lk(sampleMu_);
        sample_ = Sample{};
    }
    {
        std::lock_guard lk(errMu_);
        error_.clear();
    }
    rateHz_ = 0.0;
    auto fail = [&](const std::string& e) {
        logx::warn("head tracker: " + e);
        std::lock_guard lk(errMu_);
        error_ = e;
        return false;
    };

    switch (cfg.source) {
        case Source::None:
            return true;
        case Source::HeadphoneSensor: {
            std::wstring path = cfg.sensorPath;
            if (path.empty()) {
                const auto found = HidHeadSensor::enumerate();
                if (found.empty()) return fail("no headphone head-tracking sensor found");
                path = found.front().path;
            }
            hid_ = std::make_unique<HidHeadSensor>();
            std::string err;
            if (!hid_->start(path, *this, &err)) {
                hid_.reset();
                return fail("headphone sensor: " + err);
            }
            logx::info("head tracker: headphone sensor " + hid_->name());
            return true;
        }
        case Source::OpenTrackUdp: {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return fail("WSAStartup failed");
            SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == INVALID_SOCKET) {
                WSACleanup();
                return fail("socket failed");
            }
            BOOL reuse = TRUE;
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
            DWORD timeoutMs = 200;  // lets the thread notice stop() promptly
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(cfg.udpPort);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                const int e = WSAGetLastError();
                closesocket(s);
                WSACleanup();
                return fail("UDP port " + std::to_string(cfg.udpPort) + " is in use (" + std::to_string(e) +
                            "); close OpenTrack's own UDP input or pick another port");
            }
            socket_ = static_cast<uintptr_t>(s);
            stop_ = false;
            thread_ = std::thread(&HeadTracker::udpMain, this);
            logx::info("head tracker: listening on UDP " + std::to_string(cfg.udpPort));
            return true;
        }
    }
    return false;
}

void HeadTracker::stop() {
    if (hid_) {
        hid_->stop();
        hid_.reset();
    }
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (socket_ != ~uintptr_t(0)) {
        closesocket(static_cast<SOCKET>(socket_));
        socket_ = ~uintptr_t(0);
        WSACleanup();
    }
}

void HeadTracker::udpMain() {
    const SOCKET s = static_cast<SOCKET>(socket_);
    char buf[512];
    while (!stop_) {
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n == SOCKET_ERROR) continue;  // timeout (or transient error): re-check stop_
        Orientation o;
        if (n == 48) {
            // OpenTrack "UDP over network": double[6] = x, y, z, yaw, pitch, roll; yaw + = look left.
            double v[6];
            std::memcpy(v, buf, sizeof(v));
            if (!std::isfinite(v[3]) || !std::isfinite(v[4]) || !std::isfinite(v[5])) continue;
            o = {static_cast<float>(-v[3]), static_cast<float>(v[4]), static_cast<float>(v[5])};
        } else if (n == 50 && (static_cast<uint8_t>(buf[1]) & 0x02)) {
            // FreePIE UDP: {u8 pad, u8 flags, float[12]}; orientation in fl[0..2] (radians, OpenTrack order/signs).
            float f[3];
            std::memcpy(f, buf + 2, sizeof(f));
            if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2])) continue;
            constexpr float r2d = 57.2957795f;
            o = {-f[0] * r2d, f[1] * r2d, f[2] * r2d};
        } else {
            continue;
        }
        publish(o);
    }
}

void HeadTracker::publish(const Orientation& in) {
    Orientation o = in;
    if (cfg_.invertYaw) o.yaw = -o.yaw;
    if (cfg_.invertPitch) o.pitch = -o.pitch;
    if (cfg_.invertRoll) o.roll = -o.roll;
    const int64_t now = qpcNow();

    std::lock_guard lk(sampleMu_);
    if (sample_.qpc != 0) {
        const double dt = qpcSeconds(now - sample_.qpc);
        if (dt > 1e-4 && dt < 0.5) {
            // Smoothed angular velocity for prediction; sensor noise is large at 25-100 Hz, so blend.
            const float a = 0.4f;
            sample_.vel.yaw += a * (wrap180(o.yaw - sample_.o.yaw) / static_cast<float>(dt) - sample_.vel.yaw);
            sample_.vel.pitch += a * ((o.pitch - sample_.o.pitch) / static_cast<float>(dt) - sample_.vel.pitch);
            sample_.vel.roll += a * ((o.roll - sample_.o.roll) / static_cast<float>(dt) - sample_.vel.roll);
            rateHz_ = 0.9 * rateHz_.load() + 0.1 / dt;
        } else {
            sample_.vel = {};
        }
    }
    sample_.o = o;
    sample_.qpc = now;
}

bool HeadTracker::current(Orientation& out) const {
    {
        std::unique_lock lk(sampleMu_, std::try_to_lock);
        if (lk.owns_lock()) {
            renderCopy_ = sample_;
            renderCenter_ = center_;
        }
    }
    const Sample& s = renderCopy_;
    const Orientation& c = renderCenter_;
    if (s.qpc == 0) return false;
    const double age = qpcSeconds(qpcNow() - s.qpc);
    if (age > kStaleSec) return false;
    const float predictionMs = predictionMs_.load();
    const float ahead = static_cast<float>(std::min(age + predictionMs / 1000.0, kMaxExtrapolateSec));
    const bool predict = predictionMs > 0.0f;
    out.yaw = wrap180(s.o.yaw + (predict ? s.vel.yaw * ahead : 0.0f) - c.yaw);
    out.pitch = std::clamp(s.o.pitch + (predict ? s.vel.pitch * ahead : 0.0f) - c.pitch, -89.0f, 89.0f);
    out.roll = wrap180(s.o.roll + (predict ? s.vel.roll * ahead : 0.0f) - c.roll);
    return true;
}

void HeadTracker::recenter() {
    std::lock_guard lk(sampleMu_);
    center_ = sample_.o;
    logx::info("head tracker: recentered");
}

TrackerStatus HeadTracker::status() const {
    TrackerStatus st;
    {
        std::lock_guard lk(sampleMu_);
        st.raw = sample_.o;
        st.receiving = sample_.qpc != 0 && qpcSeconds(qpcNow() - sample_.qpc) < kStaleSec;
    }
    st.running = cfg_.source != Source::None && (thread_.joinable() || hid_ != nullptr);
    st.rateHz = st.receiving ? rateHz_.load() : 0.0;
    st.device = cfg_.source == Source::HeadphoneSensor ? (hid_ ? hid_->name() : std::string())
                : cfg_.source == Source::OpenTrackUdp ? "udp:" + std::to_string(cfg_.udpPort)
                                                       : std::string();
    std::lock_guard lk(errMu_);
    st.error = error_;
    return st;
}

}  // namespace track
