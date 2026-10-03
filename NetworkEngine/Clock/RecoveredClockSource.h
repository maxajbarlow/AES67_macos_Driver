/// @file RecoveredClockSource.h
/// @brief Clock source that locks the device clock to a received stream (phase 3).

#pragma once

#include "ClockServo.h"
#include "HostTime.h"
#include <cstdint>
#include <functional>
#include <mutex>

namespace AES67 {

/// Chooses a reference stream and steers the device's media clock to it.
///
/// Receive threads report every accepted packet's margin; only the reference
/// stream's reports reach the servo. The first stream to report becomes the
/// reference. If it falls silent for kTakeoverSilenceSeconds, the next stream
/// to report takes over at the current rate (holdover, no step). Rate changes
/// are published through the writer, which the device implements as
/// MediaClock::setRate under its clock write lock.
///
/// The writer runs under this object's lock, so once reset() returns no rate
/// computed before it can still be written. The writer must not call back
/// into this object, and callers of reset() must not hold a lock the writer
/// takes.
///
/// Thread-safe: called from every stream's receive thread (never the IO thread).
class RecoveredClockSource {
public:
    /// Called with (host time, rate ratio) whenever the servo updates the rate.
    using RateWriter = std::function<void(uint64_t hostTime, double ratio)>;

    static constexpr double kTakeoverSilenceSeconds = 1.0;

    RecoveredClockSource(ClockServo::Config config, RateWriter writer,
                         HostTimebase timebase = HostTimebase::current());

    /// An accepted packet from `stream` arrived at `arrivalHostTime` with `margin` samples.
    void observe(const void* stream, uint64_t arrivalHostTime, double margin);

    /// `stream`'s margins jumped (its placement re-anchored, the shared
    /// network mapping moved, or the local timeline restarted): re-learn the
    /// reference margin without changing the rate.
    void streamReanchored(const void* stream);

    /// `stream` stopped: release the reference if it held it (rate is held).
    void streamStopped(const void* stream);

    /// The device timeline restarted at the nominal rate: start over.
    void reset();

    const void* reference() const;
    double ratio() const;

private:
    double secondsOf(uint64_t hostTime) const;

    const RateWriter writer_;
    const HostTimebase timebase_;

    mutable std::mutex mutex_;
    ClockServo servo_;
    const void* reference_{nullptr};
    uint64_t lastReferenceArrival_{0};
};

} // namespace AES67
