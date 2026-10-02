//
// ClockProbeClient.cpp
// AES67 macOS Driver - Spike S2 client
//
// Runs IO on the "AES67 S2 Clock Probe" device for 35 s and checks that Core
// Audio follows the device's clock model: the measured rate should follow
// the probe's schedule (+300 ppm, phase step at 15 s, ramp to -300 ppm), the
// input ramp should be continuous except for the one deliberate step, and
// there should be no overloads.
//

#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr double kNominalRate = 48000.0;
constexpr double kRunSeconds = 35.0;

struct Callback {
    uint64_t hostTime;
    double sampleTime;
    UInt32 frames;
    bool rampContinuous;  // input ramp continued from the previous callback
};

struct State {
    std::vector<Callback> callbacks;
    std::atomic<size_t> count{0};
    double lastRampValue = -1.0;
    std::atomic<uint32_t> overloads{0};
};

double ticksToSeconds(uint64_t ticks) {
    static const double scale = [] {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        return static_cast<double>(tb.numer) / tb.denom / 1e9;
    }();
    return static_cast<double>(ticks) * scale;
}

double scheduledPpm(double seconds) {
    if (seconds < 15.0) return 300.0;
    if (seconds < 30.0) return 300.0 - 600.0 * (seconds - 15.0) / 15.0;
    return -300.0;
}

OSStatus ioProc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* input, const AudioTimeStamp* inputTime,
                AudioBufferList*, const AudioTimeStamp*, void* context) {
    auto* state = static_cast<State*>(context);
    const size_t index = state->count.load(std::memory_order_relaxed);
    if (index >= state->callbacks.size() || !input || input->mNumberBuffers == 0) {
        return noErr;
    }

    const auto* samples = static_cast<const Float32*>(input->mBuffers[0].mData);
    const UInt32 frames = input->mBuffers[0].mDataByteSize / sizeof(Float32);
    bool continuous = true;
    double previous = state->lastRampValue;
    for (UInt32 i = 0; i < frames; ++i) {
        if (previous >= 0.0) {
            double step = samples[i] - previous;
            if (step < -0.5) step += 1.0;  // ramp wraps every 48000 samples
            if (std::fabs(step - 1.0 / 48000.0) > 1e-6) continuous = false;
        }
        previous = samples[i];
    }
    state->lastRampValue = previous;

    state->callbacks[index] = {inputTime->mHostTime, inputTime->mSampleTime, frames, continuous};
    state->count.store(index + 1, std::memory_order_release);
    return noErr;
}

OSStatus overloadListener(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* context) {
    static_cast<State*>(context)->overloads.fetch_add(1);
    return noErr;
}

AudioObjectID findProbeDevice() {
    CFStringRef uid = CFSTR("com.aes67driver.s2probe.device");
    AudioObjectID device = kAudioObjectUnknown;
    AudioValueTranslation translation{&uid, sizeof(uid), &device, sizeof(device)};
    UInt32 size = sizeof(translation);
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDeviceForUID, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &translation);
    return device;
}

template<typename T>
T deviceProperty(AudioObjectID device, AudioObjectPropertySelector selector, T fallback) {
    T value = fallback;
    UInt32 size = sizeof(value);
    AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value);
    return value;
}

// Least-squares rate (samples per second) over callbacks in [from, to) seconds
double measuredRate(const std::vector<Callback>& cbs, size_t n, double t0, double from, double to) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        const double t = ticksToSeconds(cbs[i].hostTime) - t0;
        if (t < from || t >= to) continue;
        sx += t; sy += cbs[i].sampleTime; sxx += t * t; sxy += t * cbs[i].sampleTime;
        ++count;
    }
    if (count < 3) return 0.0;
    return (count * sxy - sx * sy) / (count * sxx - sx * sx);
}

} // namespace

int main() {
    const AudioObjectID device = findProbeDevice();
    if (device == kAudioObjectUnknown) {
        std::printf("FAIL: probe device not found (is AES67ClockProbe.driver installed?)\n");
        return 1;
    }

    State state;
    state.callbacks.resize(20000);

    AudioObjectPropertyAddress overloadAddress{kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal,
                                               kAudioObjectPropertyElementMain};
    AudioObjectAddPropertyListener(device, &overloadAddress, overloadListener, &state);

    std::printf("device %u: nominal %.0f Hz\n", device,
                deviceProperty<Float64>(device, kAudioDevicePropertyNominalSampleRate, 0));

    AudioDeviceIOProcID procID = nullptr;
    if (AudioDeviceCreateIOProcID(device, ioProc, &state, &procID) != noErr || AudioDeviceStart(device, procID) != noErr) {
        std::printf("FAIL: could not start IO\n");
        return 1;
    }

    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::duration<double>(kRunSeconds)) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("  t=%4.1f s  HAL actual rate %.3f Hz  callbacks %zu\n", elapsed,
                    deviceProperty<Float64>(device, kAudioDevicePropertyActualSampleRate, 0), state.count.load());
    }

    AudioDeviceStop(device, procID);
    AudioDeviceDestroyIOProcID(device, procID);
    AudioObjectRemovePropertyListener(device, &overloadAddress, overloadListener, &state);

    const size_t n = state.count.load(std::memory_order_acquire);
    if (n < 100) {
        std::printf("FAIL: only %zu callbacks\n", n);
        return 1;
    }
    const auto& cbs = state.callbacks;
    const double t0 = ticksToSeconds(cbs[0].hostTime);

    std::printf("\nRate in 3 s windows (measured vs scheduled, ppm relative to %.0f Hz):\n", kNominalRate);
    double worstError = 0.0;
    for (double from = 1.0; from + 3.0 <= kRunSeconds - 1.0; from += 3.0) {
        if (from < 15.5 && from + 3.0 > 14.5) continue;  // skip the window containing the step
        const double rate = measuredRate(cbs, n, t0, from, from + 3.0);
        const double ppm = (rate / kNominalRate - 1.0) * 1e6;
        const double expected = scheduledPpm(from + 1.5);
        worstError = std::max(worstError, std::fabs(ppm - expected));
        std::printf("  %5.1f-%5.1f s  measured %+8.1f  scheduled %+8.1f  error %+6.1f\n", from, from + 3.0, ppm, expected,
                    ppm - expected);
    }

    size_t timelineJumps = 0;
    size_t rampBreaks = 0;
    for (size_t i = 1; i < n; ++i) {
        const double advance = cbs[i].sampleTime - cbs[i - 1].sampleTime;
        if (std::fabs(advance - cbs[i - 1].frames) > 0.5) {
            ++timelineJumps;
            std::printf("  timeline jump at t=%.3f s: sample time advanced %.1f for %u frames\n",
                        ticksToSeconds(cbs[i].hostTime) - t0, advance, cbs[i - 1].frames);
        }
        if (!cbs[i].rampContinuous) {
            ++rampBreaks;
            std::printf("  input ramp break at t=%.3f s\n", ticksToSeconds(cbs[i].hostTime) - t0);
        }
    }

    std::printf("\ncallbacks %zu, frames/callback %u, timeline jumps %zu, ramp breaks %zu, overloads %u, worst rate error %.1f ppm\n",
                n, cbs[0].frames, timelineJumps, rampBreaks, state.overloads.load(), worstError);
    const bool pass = worstError < 20.0 && timelineJumps <= 1 && rampBreaks <= 1 && state.overloads.load() == 0;
    std::printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
