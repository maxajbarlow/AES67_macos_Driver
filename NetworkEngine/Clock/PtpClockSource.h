/// @file PtpClockSource.h
/// @brief PTP as the device's clock source: a time receiver steering the media clock.

#pragma once

#include "MediaClock.h"
#include "PtpClockControl.h"
#include "../PTP/PtpTimeReceiver.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace AES67 {

/// Runs a PTP time receiver and, a few times a second, steers the media
/// clock to it (PtpClockControl). The device applies each rate through
/// RateWriter, which must ignore a rate meant for a timeline that has since
/// restarted. While active() the device must not let other sources steer.
class PtpClockSource {
public:
    /// Apply `samplesPerTick` if the clock is still on timeline `clockGeneration`.
    using RateWriter = std::function<void(uint32_t clockGeneration, double samplesPerTick)>;
    /// Lock or clock domain changed (the device tells Core Audio).
    using ChangeListener = std::function<void()>;

    struct Config {
        Ptp::TimeReceiver::Config receiver;
        PtpClockControl::Config control;
        std::chrono::milliseconds period{125};
        std::chrono::milliseconds retryPeriod{2000};  // receiver start attempts
    };

    struct Status {
        Ptp::TimeReceiver::Status receiver;
        bool active{false};
        std::optional<int64_t> offset;  // media position minus PTP time, in samples
        double phaseErrorSamples{0.0};
    };

    PtpClockSource(Config config, const MediaClock& clock, const std::atomic<double>& sampleRate, RateWriter writer,
                   ChangeListener listener = {});
    ~PtpClockSource();
    PtpClockSource(const PtpClockSource&) = delete;
    PtpClockSource& operator=(const PtpClockSource&) = delete;

    /// Start, and keep trying to start the receiver until it can (the
    /// interface may come up after Core Audio does).
    void start();
    /// The receiver is running (its interface existed and its sockets opened).
    bool receiving() const { return receiving_.load(std::memory_order_acquire); }
    void stop();

    /// PTP has the clock (until the device timeline restarts without it).
    bool active() const { return active_.load(std::memory_order_acquire); }
    /// PTP is being followed: the clock is stable and on the grandmaster.
    bool locked() const { return locked_.load(std::memory_order_acquire); }
    /// Non-zero while locked: devices on the same grandmaster and domain share it.
    uint32_t clockDomain() const { return clockDomain_.load(std::memory_order_acquire); }

    Status status() const;

    /// A stable non-zero ID for a grandmaster on a domain.
    static uint32_t clockDomainFor(const Ptp::ClockIdentity& grandmaster, uint8_t domain);

private:
    void run();
    void step();

    const Config config_;
    const MediaClock& clock_;
    const std::atomic<double>& sampleRate_;
    const RateWriter writer_;
    const ChangeListener listener_;
    Ptp::TimeReceiver receiver_;
    PtpClockControl control_;  // run thread only

    std::atomic<bool> receiving_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> locked_{false};
    std::atomic<uint32_t> clockDomain_{0};

    mutable std::mutex statusMutex_;
    Status status_;

    std::mutex wakeMutex_;
    std::condition_variable wake_;
    bool stopping_{false};
    std::thread thread_;
};

} // namespace AES67
