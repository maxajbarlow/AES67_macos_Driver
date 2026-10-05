//
// TestPtpClockControl.cpp
// AES67 macOS Driver
// Steering the media clock to PTP. A simulated master (its own epoch and
// skew) and a simulated servo estimate drive the control; the real
// MediaClock runs on the rates it sets. Phase is checked against the
// master's true time: PTP time in samples plus the control's offset must
// equal the media position. Deterministic (fixed seeds).
//

#include "../NetworkEngine/Clock/PtpClockControl.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>

using namespace AES67;

namespace {

int checksPassed = 0;
int checksFailed = 0;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (condition) {                                                       \
            ++checksPassed;                                                    \
        } else {                                                               \
            ++checksFailed;                                                    \
            std::cerr << "  FAIL: " << message << " (" << __FILE__ << ":"      \
                      << __LINE__ << ")" << std::endl;                         \
        }                                                                      \
    } while (0)

constexpr uint64_t kTaiEpochNs = 1790000000ULL * 1000000000ULL;
constexpr uint64_t kTickNs = 125000000;  // updates 8 times a second

class Simulation {
public:
    struct Settings {
        double sampleRate{48000.0};
        double skewPpm{73.0};             // the master's rate against the host's
        double deviceStartPpm{-40.0};     // the media clock's rate before PTP takes over
        double estimatePhaseNoiseNs{0.0}; // either way, on each estimate
        double estimateRateNoisePpm{0.0};
        uint32_t seed{1};
    };

    explicit Simulation(Settings s) : s_(s), rng_(s.seed) {
        hostTicks_ = timebase_.nanosToTicks(1000ULL * 1000000000ULL);
        clock.reset(hostTicks_, 5000000,
                    MediaClock::samplesPerTick(s_.sampleRate, 1.0 + s_.deviceStartPpm * 1e-6, timebase_));
    }

    MediaClock clock;
    PtpClockControl control;
    bool following{true};
    uint32_t servoGeneration{1};
    int64_t masterStepNs{0};  // moves the master's time (a step, or a new master)

    uint64_t hostNs() const { return timebase_.ticksToNanos(hostTicks_); }
    uint64_t masterNs(uint64_t host) const {
        return kTaiEpochNs + static_cast<uint64_t>(std::llround(static_cast<double>(host) * (1.0 + s_.skewPpm * 1e-6))) +
               static_cast<uint64_t>(masterStepNs);
    }

    // One update; the rate the control asked for, if any
    PtpClockControl::Update tick() {
        hostTicks_ += timebase_.nanosToTicks(kTickNs);
        PtpReference reference;
        reference.following = following;
        reference.generation = servoGeneration;
        std::uniform_real_distribution<double> unit(-1.0, 1.0);
        const uint64_t now = hostNs();
        reference.estimate.hostAnchorNs = now;
        reference.estimate.masterAnchorNs =
            masterNs(now) + static_cast<uint64_t>(std::llround(unit(rng_) * s_.estimatePhaseNoiseNs));
        reference.estimate.rate = (1.0 + s_.skewPpm * 1e-6) * (1.0 + unit(rng_) * s_.estimateRateNoisePpm * 1e-6);
        const auto update = control.update(clock.snapshot(), s_.sampleRate, hostTicks_, reference);
        if (update.samplesPerTick) {
            clock.setRate(hostTicks_, *update.samplesPerTick);
        }
        lastUpdate_ = update;
        return update;
    }

    void run(double seconds) {
        const int ticks = static_cast<int>(std::lround(seconds * 1e9 / kTickNs));
        for (int i = 0; i < ticks; ++i) tick();
    }

    // PTP time in samples plus the offset, minus the media position: the
    // truth, from the master's actual time
    double phaseErrorSamples() const {
        const MediaPosition p = ptpSamples(masterNs(hostNs()), s_.sampleRate);
        const MediaPosition m = clock.snapshot().positionAt(hostTicks_);
        return static_cast<double>(p.sample + *control.offset() - m.sample) + (p.fraction - m.fraction);
    }

    // The media clock's rate against the master's, in ppm
    double rateErrorPpm() const {
        const double nominal = MediaClock::samplesPerTick(s_.sampleRate, 1.0 + s_.skewPpm * 1e-6, timebase_);
        return (clock.snapshot().samplesPerTick / nominal - 1.0) * 1e6;
    }

    uint64_t hostTicks() const { return hostTicks_; }

private:
    Settings s_;
    std::mt19937 rng_;
    HostTimebase timebase_{HostTimebase::current()};
    uint64_t hostTicks_{0};
    PtpClockControl::Update lastUpdate_;
};

void testPtpSamples() {
    std::cout << "PTP time in samples, exactly, at PTP-epoch magnitudes" << std::endl;
    const MediaPosition p = ptpSamples(kTaiEpochNs, 48000.0);
    CHECK(p.sample == 1790000000LL * 48000 && p.fraction == 0.0, "whole seconds are whole samples");
    const MediaPosition q = ptpSamples(kTaiEpochNs + 31250, 48000.0);  // 1.5 samples
    CHECK(q.sample == 1790000000LL * 48000 + 1 && std::fabs(q.fraction - 0.5) < 1e-9, "and fractions are kept");
    const MediaPosition r = ptpSamples(kTaiEpochNs + 1, 44100.0);
    CHECK(r.sample == 1790000000LL * 44100 && std::fabs(r.fraction - 44100e-9) < 1e-12, "at 44.1 kHz too");
}

void testWaitsForLock() {
    std::cout << "Before PTP is followed, the clock is left alone" << std::endl;
    Simulation sim(Simulation::Settings{});
    sim.following = false;
    const uint32_t generation = sim.clock.snapshot().generation;
    bool touched = false;
    for (int i = 0; i < 40; ++i) touched = touched || sim.tick().samplesPerTick.has_value();
    CHECK(!touched, "no rate is set");
    CHECK(!sim.control.active(), "PTP does not have the clock");
    CHECK(sim.clock.snapshot().generation == generation, "the timeline is untouched");
}

void testTakesOverWithoutAJump(double sampleRate) {
    std::cout << "Takes over at " << sampleRate / 1000 << " kHz without a jump, then runs in phase" << std::endl;
    Simulation::Settings s;
    s.sampleRate = sampleRate;
    Simulation sim(s);
    const uint32_t generation = sim.clock.snapshot().generation;
    const MediaPosition before = sim.clock.snapshot().positionAt(sim.hostTicks());
    const auto first = sim.tick();
    CHECK(first.offsetChanged && sim.control.active(), "PTP takes the clock and picks an offset");
    CHECK(std::fabs(sim.phaseErrorSamples()) <= 0.5 + 1e-6, "an offset within half a sample of the clock as it is ("
                                                                 << sim.phaseErrorSamples() << ")");
    const MediaPosition after = sim.clock.snapshot().positionAt(sim.hostTicks());
    const double advanced = static_cast<double>(after.sample - before.sample) + (after.fraction - before.fraction);
    CHECK(std::fabs(advanced - sampleRate * 0.125 * (1.0 - 40e-6)) < 0.01,
          "the media position just carries on (" << advanced << " samples in 125 ms)");
    sim.run(10);
    CHECK(std::fabs(sim.phaseErrorSamples()) < 0.01, "in phase within 10 s (" << sim.phaseErrorSamples() << ")");
    CHECK(std::fabs(sim.rateErrorPpm()) < 0.1, "at the master's rate (" << sim.rateErrorPpm() << " ppm)");
    CHECK(sim.clock.snapshot().generation == generation, "never a new timeline: Core Audio sees no glitch");
}

void testOffsetIsTheNearestWholeSample() {
    std::cout << "Whatever the sub-sample phase, the offset leaves at most half a sample to slew" << std::endl;
    double worst = 0;
    for (int k = 0; k < 16; ++k) {
        Simulation sim(Simulation::Settings{});
        sim.masterStepNs = k * 1302;  // sixteenths of a sample at 48 kHz
        sim.tick();
        worst = std::max(worst, std::fabs(sim.phaseErrorSamples()));
    }
    CHECK(worst <= 0.5 + 1e-6, "at most half a sample (" << worst << ")");
}

void testRidesNoisyEstimates() {
    std::cout << "Noisy estimates (+/-20 us, +/-0.5 ppm): stays within a sample, rate within the slew limit" << std::endl;
    Simulation::Settings s;
    s.estimatePhaseNoiseNs = 20000;
    s.estimateRateNoisePpm = 0.5;
    Simulation sim(s);
    sim.run(10);
    double worstPhase = 0;
    double worstRate = 0;
    for (int i = 0; i < 8 * 120; ++i) {
        sim.tick();
        worstPhase = std::max(worstPhase, std::fabs(sim.phaseErrorSamples()));
        worstRate = std::max(worstRate, std::fabs(sim.rateErrorPpm()));
    }
    std::cout << "  worst phase " << worstPhase << " samples, worst rate " << worstRate << " ppm" << std::endl;
    CHECK(worstPhase < 0.5, "within half a sample over 2 minutes (" << worstPhase << ")");
    CHECK(worstRate < 20, "the rate wanders by tens of ppm at most (" << worstRate << ")");
}

void testSlewsSmallErrorsAtACappedRate() {
    std::cout << "A 0.9 ms phase error is slewed out at no more than 100 ppm" << std::endl;
    Simulation sim(Simulation::Settings{});
    sim.run(10);
    const int64_t offset = *sim.control.offset();
    sim.masterStepNs = 900000;
    double worstRate = 0;
    bool offsetChanged = false;
    for (int i = 0; i < 8 * 30; ++i) {
        offsetChanged = offsetChanged || sim.tick().offsetChanged;
        worstRate = std::max(worstRate, std::fabs(sim.rateErrorPpm()));
    }
    CHECK(!offsetChanged && *sim.control.offset() == offset, "the offset is kept");
    CHECK(worstRate <= 100.0 + 1e-6, "the rate never moves more than 100 ppm (" << worstRate << ")");
    CHECK(worstRate > 99.0, "but uses the full 100 ppm while the error is large (" << worstRate << ")");
    CHECK(std::fabs(sim.phaseErrorSamples()) < 0.01, "and the error is gone in 30 s (" << sim.phaseErrorSamples() << ")");
}

void testLargeErrorsPickANewOffset() {
    std::cout << "A 5 ms phase error picks a new offset rather than slewing for a minute" << std::endl;
    Simulation sim(Simulation::Settings{});
    sim.run(10);
    const int64_t offset = *sim.control.offset();
    const uint32_t generation = sim.clock.snapshot().generation;
    sim.masterStepNs = 5000000;
    const auto update = sim.tick();
    CHECK(update.offsetChanged, "a new offset");
    CHECK(*sim.control.offset() == offset - 240, "5 ms earlier on the media clock (" << *sim.control.offset() - offset
                                                                                       << " samples)");
    CHECK(std::fabs(sim.phaseErrorSamples()) <= 0.5 + 1e-6, "in phase again at once");
    CHECK(sim.clock.snapshot().generation == generation, "still no new timeline");
}

void testANewPtpTimeline() {
    std::cout << "A new PTP timeline (another master, 0.6 ms apart): hold the rate, then a new offset" << std::endl;
    Simulation sim(Simulation::Settings{});
    sim.run(10);
    const double rate = sim.clock.snapshot().samplesPerTick;
    const int64_t offset = *sim.control.offset();
    // The servo starts over for the new master: not followed until it locks
    sim.following = false;
    sim.servoGeneration = 2;
    sim.masterStepNs = 600000;  // under the 1 ms limit: only the new timeline itself moves the offset
    bool touched = false;
    for (int i = 0; i < 40; ++i) touched = touched || sim.tick().samplesPerTick.has_value();
    CHECK(!touched && sim.clock.snapshot().samplesPerTick == rate, "the rate is held while it is acquired");
    CHECK(sim.control.active(), "and PTP keeps the clock meanwhile");
    sim.following = true;
    const auto update = sim.tick();
    CHECK(update.offsetChanged && *sim.control.offset() == offset - 29,
          "a new offset for the new timeline (" << *sim.control.offset() - offset << " samples), not a slew");
    sim.run(10);
    CHECK(std::fabs(sim.phaseErrorSamples()) < 0.01, "in phase with it (" << sim.phaseErrorSamples() << ")");
}

void testADeviceTimelineRestart() {
    std::cout << "A device timeline restart (IO start, new sample rate): a new offset on the next update" << std::endl;
    Simulation sim(Simulation::Settings{});
    sim.run(10);
    sim.clock.reset(sim.hostTicks(), 9000000, MediaClock::samplesPerTick(48000, 1.0, HostTimebase::current()));
    sim.following = false;
    sim.tick();
    CHECK(!sim.control.active(), "the old offset is dropped with the old timeline");
    sim.following = true;
    CHECK(sim.tick().offsetChanged && sim.control.active(), "and a new one picked once PTP is followed");
    sim.run(10);
    CHECK(std::fabs(sim.phaseErrorSamples()) < 0.01, "in phase (" << sim.phaseErrorSamples() << ")");
}

} // namespace

int main() {
    testPtpSamples();
    testWaitsForLock();
    testTakesOverWithoutAJump(48000.0);
    testTakesOverWithoutAJump(96000.0);
    testTakesOverWithoutAJump(44100.0);
    testOffsetIsTheNearestWholeSample();
    testRidesNoisyEstimates();
    testSlewsSmallErrorsAtACappedRate();
    testLargeErrorsPickANewOffset();
    testANewPtpTimeline();
    testADeviceTimelineRestart();

    std::cout << "\nPTP clock control: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
