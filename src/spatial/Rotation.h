#pragma once
// Head orientation as a world->listener rotation (Steam Audio axes: +x right, +y up, -z ahead).

#include "spatial/Binaural.h"

#include <cmath>

struct HeadRotation {
    Vec3 right{1.0f, 0.0f, 0.0f}, up{0.0f, 1.0f, 0.0f}, forward{0.0f, 0.0f, -1.0f};  // head axes in world space

    // yaw + = head turned right, pitch + = looking up, roll + = right ear down (degrees).
    static HeadRotation fromYawPitchRoll(float yawDeg, float pitchDeg, float rollDeg) {
        constexpr float k = 3.14159265358979f / 180.0f;
        const float y = yawDeg * k, p = pitchDeg * k, r = rollDeg * k;
        HeadRotation h;
        h.forward = {std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p)};
        const Vec3 r0{std::cos(y), 0.0f, std::sin(y)};  // horizontal right axis after yaw
        const Vec3 u0 = cross(r0, h.forward);           // up axis after pitch
        const float c = std::cos(r), s = std::sin(r);
        h.right = {r0.x * c - u0.x * s, r0.y * c - u0.y * s, r0.z * c - u0.z * s};
        h.up = {u0.x * c + r0.x * s, u0.y * c + r0.y * s, u0.z * c + r0.z * s};
        return h;
    }

    // Direction of a world-fixed source as heard by the rotated head.
    Vec3 toListener(const Vec3& w) const {
        return {dot(w, right), dot(w, up), -dot(w, forward)};
    }

private:
    static float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    static Vec3 cross(const Vec3& a, const Vec3& b) {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
};
