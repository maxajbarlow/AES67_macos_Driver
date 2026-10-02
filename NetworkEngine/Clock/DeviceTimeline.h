/// @file DeviceTimeline.h
/// @brief Core Audio's view of the media clock: device sample time and zero timestamps.
///
/// Device sample time T is the media position measured from the current
/// timeline's origin (T = M - origin), so it starts near 0 whatever the media
/// clock's absolute value (PTP time in samples is ~1e14). RX and TX convert
/// between the two with the same snapshot, so they cannot disagree.

#pragma once

#include "MediaClock.h"
#include <cstdint>

namespace AES67 {

/// What GetZeroTimeStamp returns to the HAL.
struct ZeroTimeStamp {
    double sampleTime{0.0};
    uint64_t hostTime{0};
    uint64_t seed{0};
};

/// Device sample time at `host` on this snapshot's timeline.
inline int64_t deviceSampleTimeAt(const MediaClock::Snapshot& clock, uint64_t host) noexcept {
    return clock.sampleAt(host) - clock.origin;
}

/// The latest period boundary at or before `host`, with the host time at
/// which that boundary's sample starts. Real-time safe.
inline ZeroTimeStamp zeroTimeStampAt(const MediaClock::Snapshot& clock, uint64_t host, uint32_t period) noexcept {
    const int64_t now = deviceSampleTimeAt(clock, host);
    const auto p = static_cast<int64_t>(period);
    const int64_t boundary = (now >= 0 ? now / p : -((-now + p - 1) / p)) * p;  // floor to a multiple of p
    return {static_cast<double>(boundary), clock.hostAt(clock.origin + boundary), clock.generation};
}

} // namespace AES67
