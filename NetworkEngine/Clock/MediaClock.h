/// @file MediaClock.h
/// @brief The device's media clock: an affine map between host time and media sample position.
///
/// With PTP the media position is PTP time in samples since the PTP epoch, so
/// every device on the grandmaster agrees on it. The active clock source keeps
/// the map up to date; Core Audio's zero timestamps, RX placement and TX RTP
/// timestamps all read it. See Docs/Step2-Clocking-Plan.md.

#pragma once

#include "HostTime.h"
#include <atomic>
#include <cmath>
#include <cstdint>

namespace AES67 {

/// A media sample position with sub-sample precision. Kept as a whole sample
/// plus a fraction because a double alone loses sub-sample precision at
/// PTP-epoch magnitudes (~1e15 samples).
struct MediaPosition {
    int64_t sample{0};
    double fraction{0.0};  // [0, 1)
};

/// Lock-free media clock. One writer (the active clock source) publishes;
/// any number of readers, including the real-time IO thread, take snapshots
/// without blocking (seqlock).
class MediaClock {
public:
    /// One published state of the clock. Pure value: safe to use after the
    /// clock has moved on.
    struct Snapshot {
        uint64_t anchorHost{0};
        int64_t anchorSample{0};
        double anchorFraction{0.0};
        double samplesPerTick{0.0};
        int64_t origin{0};       // media position where this timeline started (set by reset)
        uint32_t generation{0};  // 0 = never set; changes on every discontinuity (HAL seed)

        bool valid() const noexcept { return generation != 0; }

        MediaPosition positionAt(uint64_t host) const noexcept {
            // Signed tick difference so host times before the anchor work too
            const auto ticks = static_cast<int64_t>(host - anchorHost);
            const double offset = anchorFraction + static_cast<double>(ticks) * samplesPerTick;
            const double whole = std::floor(offset);
            return {anchorSample + static_cast<int64_t>(whole), offset - whole};
        }

        int64_t sampleAt(uint64_t host) const noexcept {
            return positionAt(host).sample;
        }

        /// First host tick at or after the start of `sample`, so
        /// sampleAt(hostAt(s)) == s (requires less than one sample per tick).
        uint64_t hostAt(int64_t sample) const noexcept {
            const double offset = static_cast<double>(sample - anchorSample) - anchorFraction;
            uint64_t host = anchorHost + static_cast<uint64_t>(static_cast<int64_t>(std::ceil(offset / samplesPerTick)));
            // The division can land a tick either side of the exact boundary;
            // settle on it using the same arithmetic as sampleAt()
            while (sampleAt(host - 1) >= sample) --host;
            while (sampleAt(host) < sample) ++host;
            return host;
        }
    };

    /// Samples per host tick for a sample rate running at `rateRatio` times nominal.
    static double samplesPerTick(double sampleRate, double rateRatio, const HostTimebase& timebase) noexcept {
        return sampleRate * rateRatio / timebase.ticksPerSecond();
    }

    /// Real-time safe. Retries only while a publish is in progress.
    Snapshot snapshot() const noexcept {
        for (;;) {
            const uint32_t before = sequence_.load(std::memory_order_acquire);
            Snapshot s;
            s.anchorHost = anchorHost_.load(std::memory_order_relaxed);
            s.anchorSample = anchorSample_.load(std::memory_order_relaxed);
            s.anchorFraction = anchorFraction_.load(std::memory_order_relaxed);
            s.samplesPerTick = samplesPerTick_.load(std::memory_order_relaxed);
            s.origin = origin_.load(std::memory_order_relaxed);
            s.generation = generation_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if ((before & 1u) == 0 && sequence_.load(std::memory_order_relaxed) == before) {
                return s;
            }
        }
    }

    // Writer side: call from one thread only.

    /// Start a new timeline at `sample`, which becomes its origin (new generation).
    void reset(uint64_t host, int64_t sample, double samplesPerTick) noexcept {
        Snapshot next{host, sample, 0.0, samplesPerTick, sample, nextGeneration()};
        publish(next);
    }

    /// Change rate from `host` onwards, keeping the position and origin continuous.
    void setRate(uint64_t host, double samplesPerTick) noexcept {
        Snapshot next = rebasedAt(host);
        next.samplesPerTick = samplesPerTick;
        publish(next);
    }

    /// Jump the position by `deltaSamples` at `host` (new generation). The
    /// origin is kept, so device time jumps by the same amount.
    void step(uint64_t host, int64_t deltaSamples) noexcept {
        Snapshot next = rebasedAt(host);
        next.anchorSample += deltaSamples;
        next.generation = nextGeneration();
        publish(next);
    }

private:
    Snapshot rebasedAt(uint64_t host) const noexcept {
        Snapshot current = snapshot();
        const MediaPosition position = current.positionAt(host);
        current.anchorHost = host;
        current.anchorSample = position.sample;
        current.anchorFraction = position.fraction;
        return current;
    }

    uint32_t nextGeneration() const noexcept {
        const uint32_t next = generation_.load(std::memory_order_relaxed) + 1;
        return next == 0 ? 1 : next;  // 0 is reserved for "never set"
    }

    void publish(const Snapshot& s) noexcept {
        const uint32_t sequence = sequence_.load(std::memory_order_relaxed);
        sequence_.store(sequence + 1, std::memory_order_relaxed);  // odd: publish in progress
        std::atomic_thread_fence(std::memory_order_release);
        anchorHost_.store(s.anchorHost, std::memory_order_relaxed);
        anchorSample_.store(s.anchorSample, std::memory_order_relaxed);
        anchorFraction_.store(s.anchorFraction, std::memory_order_relaxed);
        samplesPerTick_.store(s.samplesPerTick, std::memory_order_relaxed);
        origin_.store(s.origin, std::memory_order_relaxed);
        generation_.store(s.generation, std::memory_order_relaxed);
        sequence_.store(sequence + 2, std::memory_order_release);  // even: stable
    }

    std::atomic<uint32_t> sequence_{0};
    std::atomic<uint64_t> anchorHost_{0};
    std::atomic<int64_t> anchorSample_{0};
    std::atomic<double> anchorFraction_{0.0};
    std::atomic<double> samplesPerTick_{0.0};
    std::atomic<int64_t> origin_{0};
    std::atomic<uint32_t> generation_{0};
};

} // namespace AES67
