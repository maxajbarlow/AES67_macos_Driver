//
// ClockProbe.cpp
// AES67 macOS Driver - Spike S2 (see Docs/Step2-Clocking-Plan.md)
//
// A throwaway AudioServerPlugIn publishing one input-only device whose clock
// comes from a host-time <-> sample-time model, published lock-free and
// re-anchored every 100 ms the way the step 2 servo will. While IO runs it
// follows a fixed schedule so a client can check that Core Audio obeys it:
//
//   0-15 s   +300 ppm
//   15 s     +4800-sample phase step with a new seed
//   15-30 s  ramp from +300 to -300 ppm
//   30 s+    -300 ppm
//
// Input channel 0 carries ((sampleTime mod 48000) / 48000), so the client can
// detect dropped or repeated samples. Logs: subsystem com.aes67driver.probe,
// category S2 (needs `log show --info --debug`).
//

#include <aspl/Device.hpp>
#include <aspl/Driver.hpp>
#include <aspl/IORequestHandler.hpp>
#include <aspl/Plugin.hpp>
#include <aspl/Stream.hpp>
#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr UInt32 kZeroTimeStampPeriod = 16384;  // HAL minimum is 10923
constexpr double kStepSamples = 4800.0;         // 100 ms phase step at 15 s

os_log_t probeLog() {
    static os_log_t log = os_log_create("com.aes67driver.probe", "S2");
    return log;
}

#define PROBE_LOG(...) os_log(probeLog(), __VA_ARGS__)

double ticksPerSecond() {
    static const double value = [] {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        return 1e9 * tb.denom / tb.numer;
    }();
    return value;
}

// Scheduled clock error in ppm at `seconds` after IO start
double scheduledPpm(double seconds) {
    if (seconds < 15.0) return 300.0;
    if (seconds < 30.0) return 300.0 - 600.0 * (seconds - 15.0) / 15.0;
    return -300.0;
}

// Affine map between host ticks and device sample time, published with a
// seqlock so the IO thread never blocks. Single writer (the schedule thread).
class ClockModel {
public:
    struct Snapshot {
        uint64_t anchorHost;
        double anchorSample;
        double samplesPerTick;
        uint32_t seed;

        double sampleAt(uint64_t host) const {
            return anchorSample + (static_cast<double>(host) - static_cast<double>(anchorHost)) * samplesPerTick;
        }
        uint64_t hostAt(double sample) const {
            return anchorHost + static_cast<uint64_t>(std::llround((sample - anchorSample) / samplesPerTick));
        }
    };

    void publish(const Snapshot& s) {
        sequence_.fetch_add(1, std::memory_order_acq_rel);  // odd: write in progress
        anchorHost_.store(s.anchorHost, std::memory_order_relaxed);
        anchorSample_.store(s.anchorSample, std::memory_order_relaxed);
        samplesPerTick_.store(s.samplesPerTick, std::memory_order_relaxed);
        seed_.store(s.seed, std::memory_order_relaxed);
        sequence_.fetch_add(1, std::memory_order_release);  // even: stable
    }

    Snapshot read() const {
        for (;;) {
            const uint32_t before = sequence_.load(std::memory_order_acquire);
            Snapshot s{anchorHost_.load(std::memory_order_relaxed), anchorSample_.load(std::memory_order_relaxed),
                       samplesPerTick_.load(std::memory_order_relaxed), seed_.load(std::memory_order_relaxed)};
            std::atomic_thread_fence(std::memory_order_acquire);
            if ((before & 1) == 0 && sequence_.load(std::memory_order_relaxed) == before) {
                return s;
            }
        }
    }

private:
    std::atomic<uint32_t> sequence_{0};
    std::atomic<uint64_t> anchorHost_{0};
    std::atomic<double> anchorSample_{0.0};
    std::atomic<double> samplesPerTick_{kSampleRate / 1e9};
    std::atomic<uint32_t> seed_{1};
};

class ProbeDevice : public aspl::Device {
public:
    using aspl::Device::Device;

protected:
    OSStatus StartIOImpl(UInt32 clientID, UInt32 startCount) override {
        if (startCount == 0) {
            const uint64_t start = mach_absolute_time();
            model_.publish({start, 0.0, rateFor(300.0), 1});
            running_ = true;
            schedule_ = std::thread([this, start] { runSchedule(start); });
            PROBE_LOG("IO start: period=%u algorithm=raw", kZeroTimeStampPeriod);
        }
        return aspl::Device::StartIOImpl(clientID, startCount);
    }

    OSStatus StopIOImpl(UInt32 clientID, UInt32 startCount) override {
        if (startCount == 0) {
            running_ = false;
            if (schedule_.joinable()) schedule_.join();
            PROBE_LOG("IO stop: zeroTimeStamp calls=%llu", static_cast<unsigned long long>(zeroCalls_.load()));
        }
        return aspl::Device::StopIOImpl(clientID, startCount);
    }

    OSStatus GetZeroTimeStampImpl(UInt32, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) override {
        const ClockModel::Snapshot model = model_.read();
        const double now = model.sampleAt(mach_absolute_time());
        const double zero = std::floor(now / kZeroTimeStampPeriod) * kZeroTimeStampPeriod;
        *outSampleTime = zero;
        *outHostTime = model.hostAt(zero);
        *outSeed = model.seed;
        zeroCalls_.fetch_add(1, std::memory_order_relaxed);
        return kAudioHardwareNoError;
    }

private:
    static double rateFor(double ppm) {
        return kSampleRate * (1.0 + ppm * 1e-6) / ticksPerSecond();
    }

    // Re-anchor every 100 ms at the current position so the timeline stays
    // continuous while the rate changes, as the real servo will.
    void runSchedule(uint64_t start) {
        bool stepped = false;
        uint32_t updates = 0;
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const uint64_t now = mach_absolute_time();
            const double seconds = static_cast<double>(now - start) / ticksPerSecond();
            ClockModel::Snapshot next = model_.read();
            next.anchorSample = next.sampleAt(now);
            next.anchorHost = now;
            next.samplesPerTick = rateFor(scheduledPpm(seconds));
            if (!stepped && seconds >= 15.0) {
                next.anchorSample += kStepSamples;
                next.seed += 1;
                stepped = true;
                PROBE_LOG("phase step +%.0f samples, seed=%u at t=%.2f s", kStepSamples, next.seed, seconds);
            }
            model_.publish(next);
            if (++updates % 50 == 0) {
                PROBE_LOG("t=%.1f s ppm=%.1f sample=%.0f", seconds, scheduledPpm(seconds), next.anchorSample);
            }
        }
    }

    ClockModel model_;
    std::atomic<bool> running_{false};
    std::thread schedule_;
    std::atomic<uint64_t> zeroCalls_{0};
};

// Input channel 0: a ramp derived from the sample time, so discontinuities show
class ProbeIOHandler : public aspl::IORequestHandler {
public:
    void OnReadClientInput(const std::shared_ptr<aspl::Client>&, const std::shared_ptr<aspl::Stream>&,
                           Float64, Float64 timestamp, void* bytes, UInt32 bytesCount) override {
        auto* samples = static_cast<Float32*>(bytes);
        const UInt32 frames = bytesCount / sizeof(Float32);
        const auto base = static_cast<int64_t>(timestamp);
        for (UInt32 i = 0; i < frames; ++i) {
            const int64_t position = base + i;
            const int64_t wrapped = ((position % 48000) + 48000) % 48000;
            samples[i] = static_cast<Float32>(wrapped) / 48000.0f;
        }
    }
};

} // namespace

extern "C" void* AES67ClockProbeCreate(CFAllocatorRef, CFUUIDRef typeUUID) {
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) {
        return nullptr;
    }

    static std::shared_ptr<aspl::Driver> driver = [] {
        auto context = std::make_shared<aspl::Context>();

        aspl::DeviceParameters params;
        params.Name = "AES67 S2 Clock Probe";
        params.Manufacturer = "AES67 Driver Project (spike)";
        params.DeviceUID = "com.aes67driver.s2probe.device";
        params.ModelUID = "com.aes67driver.s2probe";
        params.CanBeDefault = false;
        params.CanBeDefaultForSystemSounds = false;
        params.SampleRate = static_cast<UInt32>(kSampleRate);
        params.ZeroTimeStampPeriod = kZeroTimeStampPeriod;
        params.ClockAlgorithm = kAudioDeviceClockAlgorithmRaw;
        params.ClockIsStable = true;
        auto device = std::make_shared<ProbeDevice>(context, params);

        aspl::StreamParameters stream;
        stream.Direction = aspl::Direction::Input;
        stream.Format.mSampleRate = kSampleRate;
        stream.Format.mFormatID = kAudioFormatLinearPCM;
        stream.Format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        stream.Format.mBitsPerChannel = 32;
        stream.Format.mChannelsPerFrame = 1;
        stream.Format.mBytesPerFrame = sizeof(Float32);
        stream.Format.mFramesPerPacket = 1;
        stream.Format.mBytesPerPacket = sizeof(Float32);
        device->AddStreamAsync(stream);
        device->SetIOHandler(std::make_shared<ProbeIOHandler>());

        auto plugin = std::make_shared<aspl::Plugin>(context);
        plugin->AddDevice(device);
        return std::make_shared<aspl::Driver>(context, plugin);
    }();

    return driver->GetReference();
}
