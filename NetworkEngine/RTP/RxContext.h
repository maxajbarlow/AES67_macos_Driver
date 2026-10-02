/// @file RxContext.h
/// @brief What the device provides to every RTP receiver.

#pragma once

#include "RtpPlacement.h"
#include "RxRouting.h"
#include "../Clock/MediaClock.h"
#include "../Clock/RecoveredClockSource.h"
#include <atomic>
#include <cstdint>

namespace AES67 {

/// Non-owning references to device state shared by all receivers. The device
/// outlives its receivers.
struct RxContext {
    const MediaClock& clock;                      // local media timeline
    NetworkTimeMapping& networkTime;              // network time -> local media position
    RxRouting& routing;                           // where the IO thread finds receive buffers
    const std::atomic<int64_t>& linkOffsetFrames; // read point = now - link offset
    RecoveredClockSource* clockRecovery{nullptr}; // steers `clock` to a received stream (phase 3)
};

} // namespace AES67
