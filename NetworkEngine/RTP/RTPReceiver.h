/// @file RTPReceiver.h
/// @brief RTP packet receiver with L16/L24 decoding and channel mapping.

#pragma once

#include "../../Shared/Types.h"
#include "../../Shared/RingBuffer.hpp"
#include "../../Driver/SDPParser.h"
#include "../StreamChannelMapper.h"
#include "../NetworkInterfaceDetection.h"
#include "SimpleRTP.h"
#include "LockFreeCircularJitterBuffer.h"
#include "RateController.h"
#include "../../Driver/AudioThreadPriority.h"
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <chrono>

namespace AES67 {

/// Receives RTP audio packets from a multicast group and writes decoded
/// audio to device ring buffers via channel mapping.
///
/// Uses two threads: receiveLoop() for network I/O into a jitter buffer,
/// and consumeLoop() for paced readout into per-channel ring buffers.
/// Includes adaptive rate matching (P-controller) to compensate for clock drift.
class RTPReceiver {
public:
    using DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, 128>;

    /// @param sdp SDP session describing the stream to receive.
    /// @param mapping Channel mapping from stream channels to device channels.
    /// @param deviceChannels Reference to device input ring buffers.
    /// @param jitterBufferDepth Jitter buffer slots (0=default 256, clamped [32,4096], rounded to power-of-2).
    /// @param networkInterface Interface name ("en0") or IP to bind multicast. Empty = INADDR_ANY.
    RTPReceiver(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        DeviceChannelBuffers& deviceChannels,
        size_t jitterBufferDepth = 0,
        const std::string& networkInterface = ""
    );

    ~RTPReceiver();

    // Prevent copy/move
    RTPReceiver(const RTPReceiver&) = delete;
    RTPReceiver& operator=(const RTPReceiver&) = delete;

    //
    // Control
    //

    // Start receiving
    bool start();

    // Stop receiving
    void stop();

    // Check if currently receiving
    bool isRunning() const { return running_.load(); }

    //
    // Status
    //

    // Get statistics (returns a non-atomic snapshot)
    StatisticsSnapshot getStatistics() const;

    // Reset statistics
    void resetStatistics();

    // Get connection status
    bool isConnected() const;

    // Get time since last packet (milliseconds)
    int64_t getTimeSinceLastPacket() const;

    //
    // Configuration
    //

    // Update channel mapping (stops and restarts receiver)
    bool updateMapping(const ChannelMapping& newMapping);

    // Get current SDP session
    const SDPSession& getSDPSession() const { return sdp_; }

    // Get current mapping
    const ChannelMapping& getMapping() const { return mapping_; }

private:
    // Network thread function (producer - adds packets to jitter buffer)
    void receiveLoop();

    // Consumer thread function (reads from jitter buffer and writes to ring buffers)
    void consumeLoop();

    // Packet processing
    void processPacket(const RTP::RTPPacket& packet);
    bool validatePacket(const RTP::RTPPacket& packet);

    // Audio decoding (return frames written to the device, 0 if the payload was rejected)
    size_t decodeL16(const uint8_t* payload, size_t payloadSize);
    size_t decodeL24(const uint8_t* payload, size_t payloadSize);

    // Write one packet's worth of silence so a lost packet keeps its place on the timeline
    void writeSilence(size_t frameCount);

    // Channel mapping: stream audio → device channels
    void mapChannelsToDevice(const float* interleavedAudio, size_t frameCount);

    // Statistics tracking
    void updateStats(uint16_t sequenceNumber, size_t payloadSize);

    // Consumer pacing (mirrors RTPTransmitter::packetInterval_)
    std::chrono::microseconds packetInterval_;

    // Pre-fill gate: consumer waits until jitter buffer has enough packets
    // before starting paced consumption, preventing initial starvation
    std::atomic<bool> prefillComplete_{false};
    static constexpr size_t kPrefillPacketCount = 6;

    // Configuration
    SDPSession sdp_;
    ChannelMapping mapping_;
    DeviceChannelBuffers& deviceChannels_;

    // RTP socket
    RTP::RTPSocket rtpSocket_;

    // Jitter buffer for packet reordering
    LockFreeCircularJitterBuffer jitterBuffer_;

    // Threading
    std::thread receiveThread_;
    std::thread consumeThread_;
    std::atomic<bool> running_{false};

    // Expected sequence number for consumer
    std::atomic<uint32_t> expectedSequenceNumber_{0};

    // Set by the receive thread once a new source or sequence base is confirmed
    // (e.g. sender restart); holds the sequence number to resync to, or -1.
    std::atomic<int32_t> resyncRequest_{-1};

    // Source tracking (receive thread only). Packets from another SSRC, or
    // further from the playout point than the jitter buffer holds, are dropped
    // unless kSourceSwitchPackets consecutive ones agree, which marks a restart
    // rather than a late packet or a second sender on the group.
    static constexpr int kSourceSwitchPackets = 4;
    bool acceptFromCurrentSource(uint16_t sequenceNumber, uint32_t ssrc);

    // Expected sequence after resyncing to a packet: backed off by the prefill
    // depth so the placeholders play as silence while the cushion rebuilds
    static uint16_t resyncTarget(int32_t resyncSequence);
    uint32_t sourceSsrc_{0};
    uint32_t candidateSsrc_{0};
    uint16_t candidateSequence_{0};
    int candidateCount_{0};
    uint64_t rejectedPackets_{0};

    // Consume thread only: frames in the most recently decoded packet, the
    // number of placeholder sequence numbers left after a resync (not losses),
    // and underrun ticks since the last decoded packet (losses covered by an
    // underrun were already played as zeros by Core Audio)
    size_t lastFrameCount_{0};
    size_t resyncFillRemaining_{0};
    size_t underrunCredit_{0};

    // Statistics (atomic operations, no mutex needed for individual updates)
    Statistics stats_;
    std::atomic<uint16_t> lastSequenceNumber_{0};
    std::atomic<uint32_t> lastTimestamp_{0};

    // Connection state
    std::atomic<bool> connected_{false};
    std::atomic<int64_t> lastPacketTimeNs_{0};

    // RTP timestamp wraparound tracking
    std::atomic<uint32_t> firstTimestamp_{0};
    std::atomic<bool> firstTimestampSet_{false};

    // Audio buffer (reused to avoid allocations)
    std::vector<float> audioBuffer_;

    // Receive buffer for network packets (max MTU 1500 bytes)
    uint8_t receiveBuffer_[2048];

    // Jitter buffer read buffer
    uint8_t jitterReadBuffer_[1500];

    // Network interface binding
    std::string networkInterface_;   // Interface name or IP from config
    std::string resolvedInterfaceIP_; // Resolved IP address (empty = INADDR_ANY)
};

} // namespace AES67
