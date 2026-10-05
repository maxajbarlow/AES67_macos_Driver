//
// TestPtpClockSource.cpp
// AES67 macOS Driver
// PTP as the clock source, end to end on lo0: a scripted master, the real
// time receiver, and the real MediaClock steered by the source. The media
// clock must run at the master's rate and in phase with its time (through
// the source's offset), without ever starting a new timeline.
//

#include "../NetworkEngine/Clock/PtpClockSource.h"
#include "PtpTestMaster.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <thread>

using namespace AES67;
using namespace AES67::PtpTest;

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

// The device's side: a media clock, written under a lock, and only on the
// timeline a rate was computed for
struct Device {
    MediaClock clock;
    std::mutex writeMutex;
    std::atomic<double> sampleRate{48000.0};
    std::atomic<int> rateWrites{0};
    std::atomic<int> staleWrites{0};
    std::atomic<int> changes{0};

    Device() { restart(7000000); }

    void restart(int64_t at) {
        std::lock_guard<std::mutex> lock(writeMutex);
        clock.reset(hostTimeNow(), at, MediaClock::samplesPerTick(sampleRate.load(), 1.0, HostTimebase::current()));
    }

    std::unique_ptr<PtpClockSource> makeSource() {
        PtpClockSource::Config config;
        config.receiver = receiverConfig();
        return std::make_unique<PtpClockSource>(
            config, clock, sampleRate,
            [this](uint32_t generation, double samplesPerTick) {
                std::lock_guard<std::mutex> lock(writeMutex);
                if (clock.snapshot().generation != generation) {
                    ++staleWrites;
                    return;
                }
                clock.setRate(hostTimeNow(), samplesPerTick);
                ++rateWrites;
            },
            [this] { ++changes; });
    }
};

bool waitFor(const std::function<bool()>& condition, double seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return condition();
}

// The master's true time in samples plus the offset, minus the media position
double phaseErrorSamples(const Device& device, const PtpClockSource& source, const ScriptedMaster& master) {
    const uint64_t now = hostTimeNow();
    const MediaPosition ptp = ptpSamples(master.clockAt(HostTimebase::current().ticksToNanos(now)), 48000.0);
    const MediaPosition media = device.clock.snapshot().positionAt(now);
    return static_cast<double>(ptp.sample + *source.status().offset - media.sample) + (ptp.fraction - media.fraction);
}

double rateErrorPpm(const Device& device, double skewPpm) {
    const double nominal = MediaClock::samplesPerTick(48000.0, 1.0 + skewPpm * 1e-6, HostTimebase::current());
    return (device.clock.snapshot().samplesPerTick / nominal - 1.0) * 1e6;
}

void testLeavesTheClockAloneWithoutAMaster() {
    std::cout << "No master: the clock is left alone" << std::endl;
    Device device;
    auto source = device.makeSource();
    source->start();
    CHECK(waitFor([&] { return source->receiving(); }, 2), "starts receiving");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    CHECK(!source->active() && !source->locked() && source->clockDomain() == 0, "not active, not locked, no domain");
    CHECK(device.rateWrites == 0 && device.changes == 0, "no rate written, nothing to report");
    source->stop();
}

void testKeepsTryingWithoutItsInterface() {
    std::cout << "Its interface missing: keeps trying, touches nothing" << std::endl;
    Device device;
    PtpClockSource::Config config;
    config.receiver = receiverConfig();
    config.receiver.networkInterface = "aes67test9";  // no such interface
    config.retryPeriod = std::chrono::milliseconds(100);
    PtpClockSource source(config, device.clock, device.sampleRate, [&](uint32_t, double) { ++device.rateWrites; });
    source.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(!source.receiving() && !source.active() && device.rateWrites == 0, "not receiving, no rate written");
    source.stop();
}

void testFollowsAMaster() {
    std::cout << "Follows a +100 ppm master: its rate, in phase, no new timeline" << std::endl;
    Device device;
    const uint32_t generation = device.clock.snapshot().generation;
    ScriptedMaster master(ScriptedMaster::Settings{});
    auto source = device.makeSource();
    source->start();
    CHECK(waitFor([&] { return source->receiving(); }, 2), "starts receiving");
    CHECK(waitFor([&] { return source->locked(); }, 20), "locks");
    std::this_thread::sleep_for(std::chrono::seconds(3));  // slew out the takeover's half sample and settle
    const double phase = phaseErrorSamples(device, *source, master);
    const double rate = rateErrorPpm(device, 100.0);
    std::cout << "  phase " << phase << " samples, rate " << rate << " ppm" << std::endl;
    CHECK(source->active(), "PTP has the clock");
    CHECK(std::fabs(phase) < 5, "within 5 samples (~100 us) of the master's time (" << phase << ")");
    CHECK(std::fabs(rate) < 20, "at its rate within 20 ppm (" << rate << ")");
    CHECK(device.clock.snapshot().generation == generation, "the device timeline never restarted");
    CHECK(source->clockDomain() == PtpClockSource::clockDomainFor(master.identity().clock, 0) &&
              source->clockDomain() != 0,
          "the clock domain is the grandmaster's");
    CHECK(device.changes >= 1, "the device was told lock and domain changed");
    source->stop();
    CHECK(!source->locked() && source->clockDomain() == 0, "stopped: neither locked nor in a domain");
}

void testADeviceTimelineRestart() {
    std::cout << "The device timeline restarts (IO start): PTP takes it over again" << std::endl;
    Device device;
    ScriptedMaster master(ScriptedMaster::Settings{});
    auto source = device.makeSource();
    source->start();
    CHECK(waitFor([&] { return source->receiving(); }, 2), "starts receiving");
    CHECK(waitFor([&] { return source->locked(); }, 20), "locks");
    const int64_t before = *source->status().offset;
    device.restart(device.clock.snapshot().sampleAt(hostTimeNow()) + (1 << 20));
    CHECK(waitFor([&] { return source->status().offset && *source->status().offset != before; }, 2),
          "a new offset for the new timeline");
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const double phase = phaseErrorSamples(device, *source, master);
    CHECK(source->active() && std::fabs(phase) < 5, "in phase on it (" << phase << ")");
    source->stop();
}

} // namespace

int main() {
    testLeavesTheClockAloneWithoutAMaster();
    testKeepsTryingWithoutItsInterface();
    testFollowsAMaster();
    testADeviceTimelineRestart();

    std::cout << "\nPTP clock source: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
