/// @file PtpClockControl.h
/// @brief Steers the device's media clock to follow PTP.

#pragma once

#include "MediaClock.h"
#include "../PTP/PtpServo.h"
#include <cstdint>
#include <optional>

namespace AES67 {

/// What the PTP slave knows, as the device clock needs it.
struct PtpReference {
    bool following{false};    // servo Locked or in Holdover: its estimate is worth following
    uint32_t generation{0};   // the servo's timeline: a new master or a step in its time
    Ptp::Servo::Estimate estimate;
};

/// Makes the media clock run at PTP's rate and in phase with it.
///
/// Media positions stay the device's own (they only ever move forward); PTP
/// time maps onto them through an integer offset, media position minus PTP
/// time in samples, chosen when PTP takes over. So taking over never jumps
/// the device clock: the rate follows PTP's, and the sub-sample remainder is
/// pulled in by a small rate correction (at most maxSlewPpm, removing phase
/// error over slewSeconds). A new PTP timeline, a new device timeline, or a
/// phase error beyond maxPhaseErrorSeconds (the clock was left to run on its
/// own, say) picks a new offset instead.
///
/// While PTP is not following (before lock, or a new master being
/// acquired) the clock keeps its last rate. Deterministic; not thread-safe.
class PtpClockControl {
public:
    struct Config {
        double slewSeconds{2.0};
        double maxSlewPpm{100.0};
        double maxPhaseErrorSeconds{0.001};
    };

    struct Update {
        std::optional<double> samplesPerTick;  // set the media clock to this rate now
        bool offsetChanged{false};
    };

    PtpClockControl() : PtpClockControl(Config{}) {}
    explicit PtpClockControl(Config config) : config_(config) {}

    /// Call regularly (a few times per second).
    /// @param clock The media clock now; its generation tells a timeline restart.
    Update update(const MediaClock::Snapshot& clock, double sampleRate, uint64_t hostNow,
                  const PtpReference& reference);

    /// PTP has the clock: it set the offset for the clock's current timeline.
    bool active() const { return offset_.has_value(); }

    /// Media position minus PTP time in samples, while active.
    std::optional<int64_t> offset() const { return offset_; }

    /// PTP time plus the offset, minus media position, at the last update (samples).
    double phaseErrorSamples() const { return phaseError_; }

private:
    const Config config_;
    std::optional<int64_t> offset_;
    uint32_t clockGeneration_{0};
    uint32_t servoGeneration_{0};
    double phaseError_{0.0};
};

/// PTP time in samples at `sampleRate` (exact: 128-bit intermediate).
MediaPosition ptpSamples(uint64_t ptpNanoseconds, double sampleRate);

} // namespace AES67
