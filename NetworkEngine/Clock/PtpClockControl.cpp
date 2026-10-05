/// @file PtpClockControl.cpp

#include "PtpClockControl.h"
#include <algorithm>
#include <cmath>

namespace AES67 {

namespace {

constexpr uint64_t kNanosPerSecond = 1000000000ULL;

} // namespace

MediaPosition ptpSamples(uint64_t ptpNanoseconds, double sampleRate) {
    // Sample rates are whole numbers of hertz; PTP time is ~1e18 ns, so the
    // product needs 128 bits to stay exact
    const auto rate = static_cast<unsigned __int128>(std::llround(sampleRate));
    const unsigned __int128 product = static_cast<unsigned __int128>(ptpNanoseconds) * rate;
    return {static_cast<int64_t>(product / kNanosPerSecond),
            static_cast<double>(static_cast<uint64_t>(product % kNanosPerSecond)) / 1e9};
}

PtpClockControl::Update PtpClockControl::update(const MediaClock::Snapshot& clock, double sampleRate, uint64_t hostNow,
                                                const PtpReference& reference) {
    Update update;
    if (clock.generation != clockGeneration_) {
        offset_.reset();  // an offset belongs to one device timeline
        clockGeneration_ = clock.generation;
    }
    if (!reference.following) {
        return update;  // hold the rate until PTP can be followed again
    }

    const HostTimebase timebase = HostTimebase::current();
    const MediaPosition ptp = ptpSamples(reference.estimate.masterAt(timebase.ticksToNanos(hostNow)), sampleRate);
    const MediaPosition media = clock.positionAt(hostNow);
    auto phaseError = [&] {
        return static_cast<double>(ptp.sample + *offset_ - media.sample) + (ptp.fraction - media.fraction);
    };

    const bool newPtpTimeline = reference.generation != servoGeneration_;
    if (!offset_ || newPtpTimeline || std::fabs(phaseError()) > config_.maxPhaseErrorSeconds * sampleRate) {
        // The whole-sample offset nearest the clock as it is: what remains is
        // under half a sample, slewed out below
        offset_ = media.sample - ptp.sample + std::llround(media.fraction - ptp.fraction);
        servoGeneration_ = reference.generation;
        update.offsetChanged = true;
    }
    phaseError_ = phaseError();

    // PTP's rate, corrected to remove the phase error over slewSeconds
    const double maxSlew = config_.maxSlewPpm * 1e-6;
    const double correction = std::clamp(phaseError_ / (config_.slewSeconds * sampleRate), -maxSlew, maxSlew);
    update.samplesPerTick = MediaClock::samplesPerTick(sampleRate, reference.estimate.rate * (1.0 + correction), timebase);
    return update;
}

} // namespace AES67
