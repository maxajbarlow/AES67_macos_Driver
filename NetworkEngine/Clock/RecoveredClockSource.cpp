/// @file RecoveredClockSource.cpp

#include "RecoveredClockSource.h"

namespace AES67 {

RecoveredClockSource::RecoveredClockSource(ClockServo::Config config, RateWriter writer, HostTimebase timebase)
    : writer_(std::move(writer))
    , timebase_(timebase)
    , servo_(config)
{
}

double RecoveredClockSource::secondsOf(uint64_t hostTime) const {
    return static_cast<double>(hostTime) / timebase_.ticksPerSecond();
}

void RecoveredClockSource::observe(const void* stream, uint64_t arrivalHostTime, double margin) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reference_ == nullptr) {
        reference_ = stream;
    } else if (stream != reference_) {
        const bool referenceSilent = arrivalHostTime > lastReferenceArrival_ &&
            secondsOf(arrivalHostTime - lastReferenceArrival_) > kTakeoverSilenceSeconds;
        if (!referenceSilent) {
            return;
        }
        // Holdover handover: keep the current rate, re-learn the margin
        reference_ = stream;
        servo_.reacquire();
    }
    lastReferenceArrival_ = arrivalHostTime;
    const std::optional<double> ratio = servo_.observe(secondsOf(arrivalHostTime), margin);
    if (ratio && writer_) {
        writer_(arrivalHostTime, *ratio);
    }
}

void RecoveredClockSource::streamReanchored(const void* stream) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream == reference_) {
        servo_.reacquire();
    }
}

void RecoveredClockSource::streamStopped(const void* stream) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream == reference_) {
        reference_ = nullptr;
        servo_.reacquire();
    }
}

void RecoveredClockSource::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    servo_.reset();
    reference_ = nullptr;
    lastReferenceArrival_ = 0;
}

const void* RecoveredClockSource::reference() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reference_;
}

double RecoveredClockSource::ratio() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return servo_.ratio();
}

} // namespace AES67
