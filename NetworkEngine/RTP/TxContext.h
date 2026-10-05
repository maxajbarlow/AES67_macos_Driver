/// @file TxContext.h
/// @brief What the device provides to every RTP transmitter.

#pragma once

#include "RtpPlacement.h"
#include "RxRouting.h"
#include "../Clock/MediaClock.h"

namespace AES67 {

/// Non-owning references to device state shared by all transmitters. The
/// device outlives its transmitters.
struct TxContext {
    const MediaClock& clock;  // media position of each packet and when it is due
    TxRouting& routing;       // where the IO thread writes the output mix
    // Fixed at PTP's offset while PTP has the clock: RTP timestamps are then
    // PTP time (plus the SDP's mediaclk offset). Null: never PTP.
    const NetworkTimeMapping* networkTime{nullptr};
};

} // namespace AES67
