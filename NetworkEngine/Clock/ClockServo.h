/// @file ClockServo.h
/// @brief Steers the media clock rate so a received stream's margin stays constant.
///
/// A stream's margin is how far ahead of the playout point its packets land.
/// If the local clock runs fast relative to the sender, margins shrink; if
/// slow, they grow. Holding the margin constant therefore locks the local
/// media clock to the sender's clock, so drift never reaches the playout
/// buffer. See Docs/Step2-Clocking-Plan.md (phase 3).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

namespace AES67 {

/// PI servo on the margin of the least-delayed packet in each window.
/// Network jitter only ever delays packets, so the per-window maximum margin
/// tracks the clocks rather than the jitter. Under heavy jitter a window can
/// hold no undelayed packet at all and read low, so the servo acts on the
/// median of the last kMedianWindows maxima: one starved window cannot kick
/// the rate. Deterministic and not thread-safe.
class ClockServo {
public:
    static constexpr std::size_t kMedianWindows = 5;

    struct Config {
        double sampleRate{48000.0};
        double naturalFrequency{0.1};    // rad/s: settles in ~40 s, ignores jitter
        double dampingRatio{1.0};        // critically damped: no overshoot
        double windowSeconds{0.1};       // margin measurement window
        double maxRatioOffset{2000e-6};  // never steer beyond +/-2000 ppm
    };

    // Delegating rather than `Config config = Config{}`: a nested struct's
    // default member initializers are not usable in the enclosing class's
    // default arguments.
    ClockServo() noexcept : ClockServo(Config{}) {}

    explicit ClockServo(Config config) noexcept
        : config_(config)
        // Plant: margin error changes at sampleRate * (ratio - senderRatio) samples/s
        , proportionalGain_(2.0 * config.dampingRatio * config.naturalFrequency / config.sampleRate)
        , integralGain_(config.naturalFrequency * config.naturalFrequency / config.sampleRate)
    {
    }

    /// Record a packet's margin (samples) at `seconds`. Returns the new clock
    /// rate ratio (local / nominal) when a measurement window completes.
    std::optional<double> observe(double seconds, double margin) noexcept {
        if (!windowOpen_) {
            openWindow(seconds, margin);
            return std::nullopt;
        }
        if (seconds - windowStart_ < config_.windowSeconds) {
            windowMax_ = std::max(windowMax_, margin);
            return std::nullopt;
        }

        // Window complete: its least-delayed packet is the raw measurement
        const double measured = pushWindowMax(windowMax_);
        openWindow(seconds, margin);

        if (!acquired_) {
            reference_ = measured;
            acquired_ = true;
            return ratio_;
        }

        // Positive error: margins shrinking, the local clock is fast: slow it
        const double error = reference_ - measured;
        const double integralLimit = config_.maxRatioOffset / integralGain_;
        // Integrate over the nominal window, not the time since it opened: after
        // an outage that would weigh one stale error by the outage's length
        integral_ = std::clamp(integral_ + error * config_.windowSeconds, -integralLimit, integralLimit);
        lastError_ = error;
        ratio_ = 1.0 - std::clamp(proportionalGain_ * error + integralGain_ * integral_,
                                  -config_.maxRatioOffset, config_.maxRatioOffset);
        return ratio_;
    }

    /// Forget everything: nominal rate, no reference.
    void reset() noexcept {
        windowOpen_ = false;
        recentNext_ = 0;
        recentCount_ = 0;
        acquired_ = false;
        integral_ = 0.0;
        lastError_ = 0.0;
        ratio_ = 1.0;
    }

    /// Keep the current rate but re-learn the margin reference (the stream
    /// re-anchored or a new reference took over). Bumpless: the proportional
    /// term is folded into the integral so the rate does not jump.
    void reacquire() noexcept {
        integral_ += proportionalGain_ * lastError_ / integralGain_;
        lastError_ = 0.0;
        windowOpen_ = false;
        recentNext_ = 0;
        recentCount_ = 0;
        acquired_ = false;
    }

    double ratio() const noexcept { return ratio_; }
    bool acquired() const noexcept { return acquired_; }

private:
    void openWindow(double seconds, double margin) noexcept {
        windowOpen_ = true;
        windowStart_ = seconds;
        windowMax_ = margin;
    }

    /// Record a window maximum; returns the median of the recent maxima.
    double pushWindowMax(double value) noexcept {
        recent_[recentNext_] = value;
        recentNext_ = (recentNext_ + 1) % kMedianWindows;
        recentCount_ = std::min(recentCount_ + 1, kMedianWindows);
        // Until the ring fills (recentNext_ restarts at 0 on reset and
        // reacquire), the filled slots are exactly the first recentCount_
        std::array<double, kMedianWindows> sorted{};
        std::copy_n(recent_.begin(), recentCount_, sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(recentCount_));
        return sorted[recentCount_ / 2];
    }

    const Config config_;
    const double proportionalGain_;
    const double integralGain_;

    bool windowOpen_{false};
    double windowStart_{0.0};
    double windowMax_{0.0};

    std::array<double, kMedianWindows> recent_{};
    std::size_t recentNext_{0};
    std::size_t recentCount_{0};

    bool acquired_{false};
    double reference_{0.0};
    double integral_{0.0};
    double lastError_{0.0};
    double ratio_{1.0};
};

} // namespace AES67
