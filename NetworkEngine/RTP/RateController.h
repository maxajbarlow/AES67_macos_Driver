/// @file RateController.h
/// @brief Locks RTPReceiver's consume thread to the sender's packet rate.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace AES67 {

/// PI controller that paces the consume thread so the jitter buffer holds a
/// steady depth. Holding depth constant means packets leave at the rate they
/// arrive, so the consume thread runs on the sender's clock rather than the
/// Mac's.
///
/// It cannot correct drift between the sender and Core Audio: pacing only moves
/// audio from the jitter buffer into the device ring buffer, and never changes
/// how much audio is buffered in total. That needs media clock recovery or
/// resampling.
///
/// Not thread-safe: use from the consume thread only.
class RateController {
public:
    static constexpr size_t kSamplesPerUpdate = 48;     // average depth over ~48 ticks
    static constexpr double kMaxRateAdjustment = 0.005; // +/- 0.5% (5000 ppm)

    // Loop dynamics: natural frequency (rad/s) and damping ratio.
    // Slow enough to ignore packet arrival jitter, fast enough to settle in ~1 min.
    static constexpr double kNaturalFrequency = 0.1;
    static constexpr double kDampingRatio = 0.8;

    /// @param tickInterval Nominal consume interval (one packet per tick).
    /// @param targetDepth Jitter buffer depth to hold, in packets.
    explicit RateController(std::chrono::microseconds tickInterval = std::chrono::microseconds(1000),
                            size_t targetDepth = 6)
        : targetDepth_(static_cast<double>(targetDepth))
        , updatePeriodSeconds_(kSamplesPerUpdate * secondsOf(tickInterval))
    {
        // Plant: depth changes by (1 / tick) packets per second per unit of rate adjustment.
        const double plantGain = 1.0 / secondsOf(tickInterval);
        proportionalGain_ = 2.0 * kDampingRatio * kNaturalFrequency / plantGain;
        integralGain_ = kNaturalFrequency * kNaturalFrequency / plantGain;
    }

    /// Record the jitter buffer depth (packets) observed after this tick's consume.
    void addDepthSample(double depthPackets) {
        depthSum_ += depthPackets;
        if (++samplesSinceUpdate_ < kSamplesPerUpdate) {
            return;
        }

        // Positive error: buffer deeper than target, so consume faster.
        const double error = depthSum_ / static_cast<double>(samplesSinceUpdate_) - targetDepth_;
        samplesSinceUpdate_ = 0;
        depthSum_ = 0.0;

        // Integrate with anti-windup: the integral term alone never exceeds the clamp.
        const double integralLimit = kMaxRateAdjustment / integralGain_;
        integral_ = std::clamp(integral_ + error * updatePeriodSeconds_, -integralLimit, integralLimit);

        rateAdjustment_ = std::clamp(proportionalGain_ * error + integralGain_ * integral_,
                                     -kMaxRateAdjustment, kMaxRateAdjustment);
    }

    /// Interval until the next consume tick (shorter when consuming faster).
    std::chrono::nanoseconds nextInterval(std::chrono::microseconds nominal) const {
        const double nanoseconds = std::chrono::duration<double, std::nano>(nominal).count() /
                                   (1.0 + rateAdjustment_);
        return std::chrono::nanoseconds(static_cast<std::chrono::nanoseconds::rep>(nanoseconds + 0.5));
    }

    /// Fractional consume-rate adjustment (+ = faster than nominal).
    double adjustment() const { return rateAdjustment_; }

    void reset() {
        rateAdjustment_ = 0.0;
        integral_ = 0.0;
        depthSum_ = 0.0;
        samplesSinceUpdate_ = 0;
    }

private:
    static double secondsOf(std::chrono::microseconds interval) {
        return std::chrono::duration<double>(interval).count();
    }

    const double targetDepth_;
    const double updatePeriodSeconds_;
    double proportionalGain_{0.0};
    double integralGain_{0.0};

    double rateAdjustment_{0.0};
    double integral_{0.0};
    double depthSum_{0.0};
    size_t samplesSinceUpdate_{0};
};

} // namespace AES67
