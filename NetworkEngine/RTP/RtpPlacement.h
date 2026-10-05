/// @file RtpPlacement.h
/// @brief Where a received RTP packet belongs on the device's media timeline.
///
/// An AES67 RTP timestamp, minus the stream's SDP mediaclk offset, is network
/// media time. NetworkTimeMapping maps network media time to the device's local
/// media position; with PTP (step 2 phase 4) that map is fixed, and until then
/// it is anchored to packet arrival and re-anchored when the clocks drift. One
/// mapping is shared by all streams, so streams from senders on the same PTP
/// grandmaster stay sample-aligned. See Docs/Step2-Clocking-Plan.md.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace AES67 {

/// The offset in an SDP's a=mediaclk:direct=<offset> (0 if none): RTP
/// timestamp minus network media time (PTP time in samples, with PTP).
inline uint32_t parseMediaClockOffset(const std::string& mediaClockType) {
    const auto at = mediaClockType.find("direct=");
    if (at == std::string::npos) {
        return 0;
    }
    return static_cast<uint32_t>(std::strtoul(mediaClockType.c_str() + at + 7, nullptr, 10));
}

/// Device-wide offset from network media time to local media position.
/// Read by every stream's receive thread; written rarely (anchor, re-anchor).
/// With PTP the device fixes it at PTP's offset: network time (RTP minus the
/// stream's mediaclk offset) is then PTP time, and placement never moves it.
class NetworkTimeMapping {
public:
    static constexpr int64_t kUnset = INT64_MIN;

    bool anchored() const noexcept { return offset() != kUnset; }
    int64_t offset() const noexcept { return offset_.load(std::memory_order_acquire); }

    /// Anchor if no stream has yet; returns the offset in force afterwards.
    int64_t anchorIfUnset(int64_t offset) noexcept {
        int64_t expected = kUnset;
        offset_.compare_exchange_strong(expected, offset, std::memory_order_acq_rel);
        return offset_.load(std::memory_order_acquire);
    }

    void reanchor(int64_t offset) noexcept { offset_.store(offset, std::memory_order_release); }

    /// PTP's offset (media position minus PTP time in samples): network
    /// time's true place, which placement never moves.
    void fix(int64_t offset) noexcept {
        offset_.store(offset, std::memory_order_release);
        fixed_.store(true, std::memory_order_release);
    }

    /// The mapping is PTP's (until the next reset()).
    bool fixed() const noexcept { return fixed_.load(std::memory_order_acquire) && anchored(); }

    /// Forget the anchor (the local timeline restarted).
    void reset() noexcept {
        fixed_.store(false, std::memory_order_release);
        offset_.store(kUnset, std::memory_order_release);
    }

private:
    std::atomic<int64_t> offset_{kUnset};
    std::atomic<bool> fixed_{false};
};

/// Places one stream's packets. Receive thread only (not thread-safe).
///
/// A packet is playable when its end lands after the read point (local now
/// minus the link offset) and no more than two link offsets ahead of it.
/// Packets outside that window, or from another SSRC, are dropped unless a
/// sustained run agrees with itself, which marks a sender restart or clock
/// drift rather than a stray packet; the stream then follows the new source,
/// re-anchoring if its timestamps do not fit. A run must hold at least
/// kSourceSwitchPackets packets covering Config::sourceSwitchFrames (50 ms by
/// default) and half a link offset, with margins within a quarter of a link
/// offset of each other. A timeline change persists while a stall does not:
/// a stalled sender's late packets can keep pace for a few packets before
/// its catch-up burst (whose margins then grow by a packet each), so a
/// transient stall drops its late packets but never moves the timeline.
class RtpPlacement {
public:
    static constexpr int kSourceSwitchPackets = 4;
    static constexpr double kSourceSwitchSeconds = 0.05;

    struct Config {
        uint32_t mediaClockOffset{0};  // from the SDP a=mediaclk:direct=
        int64_t linkOffsetFrames{384};
        int64_t sourceSwitchFrames{2400};  // kSourceSwitchSeconds at the stream's rate (48 kHz here)
    };

    enum class Verdict { Accepted, Late, Early, Foreign };

    struct Result {
        Verdict verdict{Verdict::Late};
        int64_t position{0};   // local media position of the packet's first frame
        bool reanchored{false};
    };

    RtpPlacement(Config config, NetworkTimeMapping& mapping) noexcept;

    /// @param localArrival Local media position at which the packet arrived.
    Result place(uint32_t rtpTimestamp, uint32_t ssrc, uint32_t frames, int64_t localArrival) noexcept;

    /// Start over (receiver restarted or the local timeline changed).
    void reset() noexcept;

    uint64_t lateDrops() const noexcept { return lateDrops_; }
    uint64_t earlyDrops() const noexcept { return earlyDrops_; }
    uint64_t foreignDrops() const noexcept { return foreignDrops_; }
    uint64_t reanchors() const noexcept { return reanchors_; }

private:
    // Network time for a 32-bit timestamp: the value nearest `reference`.
    static int64_t unwrapNear(uint32_t networkTimestamp, int64_t reference) noexcept;

    int64_t positionOf(uint32_t networkTimestamp, int64_t localArrival) const noexcept;
    int64_t marginOf(int64_t position, uint32_t frames, int64_t localArrival) const noexcept;
    bool inWindow(int64_t margin) const noexcept;

    const Config config_;
    NetworkTimeMapping& mapping_;

    bool haveSource_{false};
    uint32_t ssrc_{0};
    int64_t privateAdjust_{0};   // non-zero once this stream left the shared mapping
    bool followsShared_{true};

    int candidateCount_{0};
    uint32_t candidateSsrc_{0};
    uint32_t candidateTimestamp_{0};
    int64_t candidateFirstMargin_{0};
    int64_t candidateFrames_{0};

    uint64_t lateDrops_{0};
    uint64_t earlyDrops_{0};
    uint64_t foreignDrops_{0};
    uint64_t reanchors_{0};
};

} // namespace AES67
