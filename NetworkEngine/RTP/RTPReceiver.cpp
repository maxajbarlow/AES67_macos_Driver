//
// RTPReceiver.cpp
// AES67 macOS Driver - Build #9
// RTP packet receiver with L16/L24 decoding and channel mapping integration
//

#include "RTPReceiver.h"
#include "SimpleRTP.h"
#include "../../Driver/DebugLog.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <sys/select.h>

namespace AES67 {

RTPReceiver::RTPReceiver(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    DeviceChannelBuffers& deviceChannels,
    size_t jitterBufferDepth,
    const std::string& networkInterface
)
    : sdp_(sdp)
    , mapping_(mapping)
    , deviceChannels_(deviceChannels)
    , jitterBuffer_(jitterBufferDepth > 0 ? jitterBufferDepth
                                          : LockFreeCircularJitterBuffer::DEFAULT_BUFFER_SIZE)
    , networkInterface_(networkInterface)
{
    std::memset(&stats_, 0, sizeof(stats_));

    // Pre-allocate audio buffer to avoid allocations in receiveLoop()
    // Max 512 frames × stream channels (e.g., 512 × 8 = 4096 floats)
    const size_t maxFrames = 512;
    const size_t maxSamples = maxFrames * sdp_.numChannels;
    audioBuffer_.resize(maxSamples);

    // Calculate packet interval from SDP (mirrors RTPTransmitter constructor)
    // Priority: ptime field > framecount/sampleRate > 1ms fallback
    if (sdp_.ptime > 0) {
        packetInterval_ = std::chrono::microseconds(sdp_.ptime * 1000);
    } else if (sdp_.framecount > 0 && sdp_.sampleRate > 0) {
        uint64_t intervalUs = (static_cast<uint64_t>(sdp_.framecount) * 1000000ULL) / sdp_.sampleRate;
        packetInterval_ = std::chrono::microseconds(intervalUs);
    } else {
        packetInterval_ = std::chrono::microseconds(1000); // 1ms default for AES67
    }

    // Resolve network interface name to IP address
    if (!networkInterface_.empty()) {
        bool looksLikeIP = true;
        for (char c : networkInterface_) {
            if (c != '.' && !isdigit(c)) {
                looksLikeIP = false;
                break;
            }
        }

        if (looksLikeIP) {
            resolvedInterfaceIP_ = networkInterface_;
            AES67_LOGF("RTPReceiver: using interface IP %s directly (stream=%s)",
                       resolvedInterfaceIP_.c_str(), sdp_.sessionName.c_str());
        } else {
            resolvedInterfaceIP_ = NetworkInterfaceDetection::getInterfaceIPAddress(networkInterface_);
            if (resolvedInterfaceIP_.empty()) {
                AES67_LOGF("RTPReceiver: WARNING - failed to resolve interface '%s' to IP, "
                           "falling back to INADDR_ANY (stream=%s)",
                           networkInterface_.c_str(), sdp_.sessionName.c_str());
            } else {
                AES67_LOGF("RTPReceiver: resolved interface '%s' to IP %s (stream=%s)",
                           networkInterface_.c_str(), resolvedInterfaceIP_.c_str(),
                           sdp_.sessionName.c_str());
            }
        }
    }
}

RTPReceiver::~RTPReceiver() {
    stop();
}

bool RTPReceiver::start() {
    if (running_ || rtpSocket_.isOpen()) {
        AES67_LOGF("RTPReceiver::start: already running or socket open (stream=%s)",
                   sdp_.sessionName.c_str());
        return false; // Already running
    }

    // Validate SDP configuration
    if (sdp_.connectionAddress.empty() || sdp_.port == 0) {
        AES67_LOGF("RTPReceiver::start: invalid SDP - address='%s' port=%u (stream=%s)",
                   sdp_.connectionAddress.c_str(), sdp_.port, sdp_.sessionName.c_str());
        return false;
    }

    if (sdp_.numChannels == 0 || sdp_.numChannels > 128) {
        AES67_LOGF("RTPReceiver::start: invalid channel count %u (stream=%s)",
                   sdp_.numChannels, sdp_.sessionName.c_str());
        return false;
    }

    // Open RTP receiver socket, optionally bound to a specific interface
    const char* ifaceIP = resolvedInterfaceIP_.empty() ? nullptr : resolvedInterfaceIP_.c_str();
    if (!rtpSocket_.openReceiver(sdp_.connectionAddress.c_str(), sdp_.port, ifaceIP)) {
        AES67_LOGF("RTPReceiver::start: socket open failed for %s:%u iface=%s (stream=%s)",
                   sdp_.connectionAddress.c_str(), sdp_.port,
                   resolvedInterfaceIP_.empty() ? "ANY" : resolvedInterfaceIP_.c_str(),
                   sdp_.sessionName.c_str());
        return false;
    }

    AES67_LOGF("RTPReceiver::start: opened socket for %s:%u on interface %s (stream=%s)",
               sdp_.connectionAddress.c_str(), sdp_.port,
               resolvedInterfaceIP_.empty() ? "INADDR_ANY" : resolvedInterfaceIP_.c_str(),
               sdp_.sessionName.c_str());

    // Reset jitter buffer and prefill gate
    jitterBuffer_.reset();
    expectedSequenceNumber_.store(0, std::memory_order_relaxed);
    resyncRequest_.store(-1, std::memory_order_relaxed);
    candidateCount_ = 0;
    rejectedPackets_ = 0;
    resyncFillRemaining_ = 0;
    underrunCredit_ = 0;
    // Silence length for lost packets until the first packet is decoded
    lastFrameCount_ = sdp_.framecount > 0
        ? sdp_.framecount
        : static_cast<size_t>(std::lround(sdp_.sampleRate * std::chrono::duration<double>(packetInterval_).count()));
    prefillComplete_.store(false, std::memory_order_relaxed);

    // Start receive thread (producer - adds packets to jitter buffer)
    running_ = true;
    receiveThread_ = std::thread([this]() {
        if (!AudioThreadPriority::configureForRealTime()) {
            AES67_LOGF("RTPReceiver: failed to set RT priority on receive thread (stream=%s)",
                       sdp_.sessionName.c_str());
        }
        receiveLoop();
    });

    // Start consume thread (consumer - reads from jitter buffer and writes to ring buffers)
    consumeThread_ = std::thread([this]() {
        if (!AudioThreadPriority::configureForRealTime()) {
            AES67_LOGF("RTPReceiver: failed to set RT priority on consume thread (stream=%s)",
                       sdp_.sessionName.c_str());
        }
        consumeLoop();
    });

    return true;
}

void RTPReceiver::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }

    if (consumeThread_.joinable()) {
        consumeThread_.join();
    }

    rtpSocket_.close();
    connected_ = false;
}

StatisticsSnapshot RTPReceiver::getStatistics() const {
    // Return a consistent snapshot of atomic statistics
    return stats_.snapshot();
}

void RTPReceiver::resetStatistics() {
    // Reset all atomic counters
    stats_.packetsReceived.store(0, std::memory_order_relaxed);
    stats_.packetsLost.store(0, std::memory_order_relaxed);
    stats_.malformedPackets.store(0, std::memory_order_relaxed);
    stats_.outOfOrderPackets.store(0, std::memory_order_relaxed);
    stats_.underruns.store(0, std::memory_order_relaxed);
    stats_.overruns.store(0, std::memory_order_relaxed);
    stats_.jitterNs.store(0, std::memory_order_relaxed);
    stats_.latencyNs.store(0, std::memory_order_relaxed);
    stats_.bytesReceived.store(0, std::memory_order_relaxed);
    stats_.bytesSent.store(0, std::memory_order_relaxed);
    lastSequenceNumber_.store(0, std::memory_order_relaxed);
    lastTimestamp_.store(0, std::memory_order_relaxed);
    expectedSequenceNumber_.store(0, std::memory_order_relaxed);
    firstTimestampSet_.store(false, std::memory_order_relaxed);
    firstTimestamp_.store(0, std::memory_order_relaxed);

    // Reset jitter buffer
    jitterBuffer_.reset();
}

bool RTPReceiver::isConnected() const {
    if (!connected_) {
        return false;
    }

    // Consider disconnected if no packet in last 1 second
    int64_t lastNs = lastPacketTimeNs_.load(std::memory_order_acquire);
    if (lastNs == 0) return false;
    auto now = std::chrono::steady_clock::now();
    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    return (nowNs - lastNs) < 1000000000LL; // 1 second in ns
}

int64_t RTPReceiver::getTimeSinceLastPacket() const {
    if (!connected_) {
        return -1;
    }

    int64_t lastNs = lastPacketTimeNs_.load(std::memory_order_acquire);
    if (lastNs == 0) return -1;
    auto now = std::chrono::steady_clock::now();
    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();

    return (nowNs - lastNs) / 1000000; // ns to ms
}

bool RTPReceiver::updateMapping(const ChannelMapping& newMapping) {
    // Validate mapping
    if (newMapping.deviceChannelStart + sdp_.numChannels > 128) {
        return false;
    }

    // Stop, update, restart
    const bool wasRunning = running_;
    if (wasRunning) {
        stop();
    }

    mapping_ = newMapping;

    if (wasRunning) {
        return start();
    }

    return true;
}

void RTPReceiver::receiveLoop() {
    fd_set readfds;
    struct timeval tv;

    RTP::RTPPacket packet;

    while (running_) {
        // Set up select with 1ms timeout (responsive but not spinning)
        FD_ZERO(&readfds);
        int sockfd = rtpSocket_.getFd();
        if (sockfd < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        FD_SET(sockfd, &readfds);

        tv.tv_sec = 0;
        tv.tv_usec = 1000;  // 1ms timeout

        int ret = select(sockfd + 1, &readfds, nullptr, nullptr, &tv);

        if (ret > 0 && FD_ISSET(sockfd, &readfds)) {
            ssize_t bytesReceived = rtpSocket_.receive(packet, receiveBuffer_, sizeof(receiveBuffer_));
            if (bytesReceived > 0) {
                processPacket(packet);
            }
        }
    }
}

void RTPReceiver::consumeLoop() {
    // This thread pulls packets from the jitter buffer in sequence order
    // and writes decoded audio to the device ring buffers.
    //
    // Two phases:
    //   1. Pre-fill: wait for the jitter buffer to accumulate enough packets
    //      so that the consumer has a cushion against network jitter.
    //   2. Paced consumption: consume one packet per packetInterval_ using
    //      sleep_until (mirrors RTPTransmitter::transmitLoop pattern).

    size_t outputLength = 0;
    uint64_t presentationTime = 0;

    // ── Phase 1: Pre-fill wait ──────────────────────────────────────
    // The receive thread is already running and populating the jitter buffer.
    // We wait here until enough packets have accumulated before starting
    // paced consumption. During this time Core Audio reads empty ring buffers
    // and fills silence (existing AES67IOHandler behavior).
    while (running_ && !prefillComplete_.load(std::memory_order_relaxed)) {
        size_t buffered = jitterBuffer_.getBufferedPacketCount();
        if (buffered >= kPrefillPacketCount) {
            prefillComplete_.store(true, std::memory_order_relaxed);
            break;
        }
        std::this_thread::sleep_for(packetInterval_);
    }

    // ── Phase 2: Paced consumption via sleep_until ──────────────────
    // Target absolute wall-clock times so we don't accumulate drift from
    // variable decode/write latency. This is the same pattern used by
    // RTPTransmitter::transmitLoop().
    auto nextConsumeTime = std::chrono::steady_clock::now();

    RateController rateController(packetInterval_, kPrefillPacketCount);

    while (running_) {
        std::this_thread::sleep_until(nextConsumeTime);

        // Advance target time (with rate adjustment applied)
        nextConsumeTime += rateController.nextInterval(packetInterval_);

        // Sender restarted (or a gap longer than the jitter buffer): jump to the
        // new sequence, backed off by the prefill depth. Those earlier sequence
        // numbers were never sent, so they play as silence while the jitter
        // buffer rebuilds its cushion.
        int32_t resyncTo = resyncRequest_.load(std::memory_order_acquire);
        if (resyncTo >= 0) {
            // Publish the new expected sequence before clearing the request, so
            // the receive thread never judges packets against a stale value.
            // If a newer request arrived meanwhile the CAS fails and it is
            // applied on the next tick.
            expectedSequenceNumber_.store(resyncTarget(resyncTo), std::memory_order_release);
            resyncFillRemaining_ = kPrefillPacketCount;
            underrunCredit_ = 0;
            rateController.reset();
            resyncRequest_.compare_exchange_strong(resyncTo, -1, std::memory_order_acq_rel);
            AES67_LOGF("RTPReceiver: resynced to sequence %d (stream=%s)", resyncTo, sdp_.sessionName.c_str());
        }

        uint32_t expectedSeq = expectedSequenceNumber_.load(std::memory_order_acquire);

        bool gotPacket = jitterBuffer_.getNextPacket(
            jitterReadBuffer_,
            sizeof(jitterReadBuffer_),
            outputLength,
            presentationTime,
            expectedSeq
        );

        if (gotPacket) {
            // Successfully got the expected packet — decode and map to ring buffers
            expectedSequenceNumber_.store((expectedSeq + 1) & 0xFFFF, std::memory_order_release);

            size_t decodedFrames = 0;
            if (sdp_.encoding == "L16") {
                decodedFrames = decodeL16(jitterReadBuffer_, outputLength);
            } else if (sdp_.encoding == "L24") {
                decodedFrames = decodeL24(jitterReadBuffer_, outputLength);
            }
            if (decodedFrames > 0) {
                lastFrameCount_ = decodedFrames;
            }
            underrunCredit_ = 0;
        } else {
            // Expected packet not available
            size_t bufferedCount = jitterBuffer_.getBufferedPacketCount();

            if (bufferedCount > 0) {
                // Buffer has packets but not the one we want — packet loss.
                // Skip to the next sequence number so we don't stall.
                expectedSequenceNumber_.store((expectedSeq + 1) & 0xFFFF, std::memory_order_release);
                if (resyncFillRemaining_ > 0) {
                    // Placeholder after a resync (never sent): silence rebuilds the cushion
                    --resyncFillRemaining_;
                    writeSilence(lastFrameCount_);
                } else if (underrunCredit_ > 0) {
                    // Lost during an outage we already underran through: Core
                    // Audio played that time as zeros, so don't add it again
                    --underrunCredit_;
                    stats_.packetsLost.fetch_add(1, std::memory_order_relaxed);
                } else {
                    // Play silence in its place so this stream stays aligned
                    // with the others
                    stats_.packetsLost.fetch_add(1, std::memory_order_relaxed);
                    writeSilence(lastFrameCount_);
                }
            } else {
                // Buffer completely empty — underrun.
                // Do NOT advance sequence number; the packet may still arrive.
                stats_.underruns.fetch_add(1, std::memory_order_relaxed);
                if (underrunCredit_ < jitterBuffer_.getMaxBufferSize()) {
                    ++underrunCredit_;
                }
            }
        }

        // ── Sender rate tracking ────────────────────────────────────
        // Hold the jitter buffer at its prefill depth so consumption follows
        // the sender's packet clock (see RateController for what this can and
        // cannot correct).
        rateController.addDepthSample(static_cast<double>(jitterBuffer_.getBufferedPacketCount()));
    }
}

void RTPReceiver::processPacket(const RTP::RTPPacket& packet) {
    if (!validatePacket(packet)) {
        uint64_t count = stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed) + 1;
        // Log first occurrence and then every 100th to avoid flooding
        if (count == 1 || count % 100 == 0) {
            AES67_LOGF("RTPReceiver: malformed packet #%llu (ver=%u pt=%u size=%zu, expected pt=%u) stream=%s",
                       (unsigned long long)count, packet.header.version,
                       packet.header.payloadType, packet.payloadSize,
                       sdp_.payloadType, sdp_.sessionName.c_str());
        }
        return;
    }

    // Get RTP header info
    uint16_t sequenceNumber = packet.header.sequenceNumber;
    uint32_t timestamp = packet.header.timestamp;

    // Get payload
    uint8_t* payload = packet.payload;
    size_t payloadSize = packet.payloadSize;

    if (!payload || payloadSize == 0) {
        uint64_t count = stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1 || count % 100 == 0) {
            AES67_LOGF("RTPReceiver: empty payload in packet #%llu (seq=%u) stream=%s",
                       (unsigned long long)count, sequenceNumber, sdp_.sessionName.c_str());
        }
        return;
    }

    // Update connection state
    if (!connected_) {
        connected_ = true;
        sourceSsrc_ = packet.header.ssrc;
        // Initialize expected sequence number from first packet.
        // Use release ordering so the consume thread (which loads with
        // acquire) is guaranteed to see this initial value.
        expectedSequenceNumber_.store(sequenceNumber, std::memory_order_release);
    } else if (!acceptFromCurrentSource(sequenceNumber, packet.header.ssrc)) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    lastPacketTimeNs_.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count(),
        std::memory_order_release);

    // Update statistics
    updateStats(sequenceNumber, payloadSize);

    // Calculate presentation time from RTP timestamp delta (wraparound-safe)
    // RTP timestamps are 32-bit and wrap every ~24h at 48kHz.
    // We track the delta from the first timestamp to avoid overflow when
    // multiplying by 1e9, and to correctly handle 32-bit wraparound.
    uint64_t presentationTime = 0;
    if (sdp_.sampleRate > 0) {
        uint32_t firstTs = firstTimestamp_.load(std::memory_order_relaxed);
        if (firstTimestampSet_.load(std::memory_order_relaxed)) {
            // Wraparound-safe delta: unsigned subtraction handles 32-bit wrap
            uint32_t delta = timestamp - firstTs;
            // delta is at most 2^32-1 (~4.29e9). At 384kHz that's ~11184s.
            // 4.29e9 * 1e9 = 4.29e18 < UINT64_MAX (1.84e19), so no overflow.
            presentationTime = (static_cast<uint64_t>(delta) * 1000000000ULL) / sdp_.sampleRate;
        } else {
            // First packet — record baseline timestamp
            firstTimestamp_.store(timestamp, std::memory_order_relaxed);
            firstTimestampSet_.store(true, std::memory_order_relaxed);
            presentationTime = 0;
        }
    }

    // Add packet to jitter buffer (lock-free operation)
    // The jitter buffer will handle reordering based on sequence numbers
    if (!jitterBuffer_.addPacket(payload, payloadSize, sequenceNumber, presentationTime)) {
        // Jitter buffer full or slot already occupied
        stats_.overruns.fetch_add(1, std::memory_order_relaxed);
    }
}

uint16_t RTPReceiver::resyncTarget(int32_t resyncSequence) {
    return static_cast<uint16_t>(static_cast<uint32_t>(resyncSequence) - kPrefillPacketCount);
}

bool RTPReceiver::acceptFromCurrentSource(uint16_t sequenceNumber, uint32_t ssrc) {
    // Judge against a resync the consumer has not applied yet, if any, so
    // packets just after a source switch are not mistaken for strays
    const int32_t pending = resyncRequest_.load(std::memory_order_acquire);
    const auto expected = pending >= 0
        ? resyncTarget(pending)
        : static_cast<uint16_t>(expectedSequenceNumber_.load(std::memory_order_acquire));
    const auto window = static_cast<int32_t>(jitterBuffer_.getMaxBufferSize());
    const int32_t gap = RTP::sequenceDistance(expected, sequenceNumber);

    // Playable: from our source, at or ahead of the consumer, within the buffer.
    // Anything behind the consumer has already been played or declared lost.
    if (ssrc == sourceSsrc_ && gap >= 0 && gap <= window) {
        candidateCount_ = 0;
        return true;
    }

    // Another SSRC, a sequence number behind the consumer, or one the jitter
    // buffer cannot hold: a sender restart (RFC 3550 senders pick a random SSRC
    // and initial sequence number, which may land behind us), a very long
    // outage, a late packet, or a second sender on the group. Only a sustained
    // run of consecutive packets from the same candidate is treated as a
    // restart; anything else is dropped.
    const bool continuesCandidate = candidateCount_ > 0 && ssrc == candidateSsrc_ &&
        RTP::sequenceDistance(candidateSequence_, sequenceNumber) > 0 &&
        RTP::sequenceDistance(candidateSequence_, sequenceNumber) <= window;
    candidateCount_ = continuesCandidate ? candidateCount_ + 1 : 1;
    candidateSsrc_ = ssrc;
    candidateSequence_ = sequenceNumber;

    if (candidateCount_ >= kSourceSwitchPackets) {
        AES67_LOGF("RTPReceiver: following new source ssrc=%08x seq=%u (was ssrc=%08x, expected seq=%u) stream=%s",
                   ssrc, sequenceNumber, sourceSsrc_, expected, sdp_.sessionName.c_str());
        sourceSsrc_ = ssrc;
        candidateCount_ = 0;
        resyncRequest_.store(sequenceNumber, std::memory_order_release);
        return true;
    }

    if (++rejectedPackets_ == 1 || rejectedPackets_ % 1000 == 0) {
        AES67_LOGF("RTPReceiver: ignored packet #%llu ssrc=%08x seq=%u (playing ssrc=%08x near seq=%u) stream=%s",
                   (unsigned long long)rejectedPackets_, ssrc, sequenceNumber, sourceSsrc_, expected,
                   sdp_.sessionName.c_str());
    }
    return false;
}

bool RTPReceiver::validatePacket(const RTP::RTPPacket& packet) {
    // Check RTP version (should be 2)
    if (packet.header.version != 2) {
        return false;
    }

    // Check payload type matches SDP
    if (packet.header.payloadType != sdp_.payloadType) {
        return false;
    }

    // Check payload size is reasonable
    if (packet.payloadSize == 0 || packet.payloadSize > 1500) {
        return false;
    }

    return true;
}

size_t RTPReceiver::decodeL16(const uint8_t* payload, size_t payloadSize) {
    // L16: 16-bit big-endian signed PCM
    const size_t bytesPerSample = 2;
    const size_t bytesPerFrame = bytesPerSample * sdp_.numChannels;
    const size_t frameCount = payloadSize / bytesPerFrame;

    if (frameCount == 0 || frameCount > 512) {
        return 0; // Invalid or excessive frame count
    }

    // Ensure audio buffer is large enough
    const size_t totalSamples = frameCount * sdp_.numChannels;
    if (totalSamples > audioBuffer_.size()) {
        stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // Decode: big-endian int16 → float [-1.0, 1.0)
    for (size_t i = 0; i < totalSamples; ++i) {
        const size_t offset = i * bytesPerSample;
        if (offset + bytesPerSample > payloadSize) break;

        // Big-endian 16-bit signed (assemble as unsigned to avoid
        // implementation-defined narrowing from promoted int to int16_t)
        uint16_t rawSample = (static_cast<uint16_t>(payload[offset]) << 8) | payload[offset + 1];
        int16_t pcmSample = static_cast<int16_t>(rawSample);

        // Convert to float: divide by 32768 (2^15)
        audioBuffer_[i] = pcmSample / 32768.0f;
    }

    // Map to device channels
    mapChannelsToDevice(audioBuffer_.data(), frameCount);
    return frameCount;
}

size_t RTPReceiver::decodeL24(const uint8_t* payload, size_t payloadSize) {
    // L24: 24-bit big-endian signed PCM
    const size_t bytesPerSample = 3;
    const size_t bytesPerFrame = bytesPerSample * sdp_.numChannels;
    const size_t frameCount = payloadSize / bytesPerFrame;

    if (frameCount == 0 || frameCount > 512) {
        return 0; // Invalid or excessive frame count
    }

    // Ensure audio buffer is large enough
    const size_t totalSamples = frameCount * sdp_.numChannels;
    if (totalSamples > audioBuffer_.size()) {
        stats_.malformedPackets.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    // Decode: big-endian int24 → float [-1.0, 1.0)
    for (size_t i = 0; i < totalSamples; ++i) {
        const size_t offset = i * bytesPerSample;
        if (offset + bytesPerSample > payloadSize) break;

        // Big-endian 24-bit signed (sign-extend to 32-bit)
        // Cast to uint32_t before shifting to avoid undefined behavior
        // when high byte >= 128 causes signed int overflow on << 24
        uint32_t rawSample = (static_cast<uint32_t>(payload[offset]) << 24) |
                             (static_cast<uint32_t>(payload[offset + 1]) << 16) |
                             (static_cast<uint32_t>(payload[offset + 2]) << 8);
        int32_t pcmSample = static_cast<int32_t>(rawSample);
        pcmSample >>= 8; // Arithmetic right shift preserves sign

        // Convert to float: divide by 8388608 (2^23)
        audioBuffer_[i] = pcmSample / 8388608.0f;
    }

    // Map to device channels
    mapChannelsToDevice(audioBuffer_.data(), frameCount);
    return frameCount;
}

void RTPReceiver::writeSilence(size_t frameCount) {
    const size_t totalSamples = frameCount * sdp_.numChannels;
    if (frameCount == 0 || totalSamples > audioBuffer_.size()) {
        return;
    }
    std::fill_n(audioBuffer_.begin(), totalSamples, 0.0f);
    mapChannelsToDevice(audioBuffer_.data(), frameCount);
}

void RTPReceiver::mapChannelsToDevice(const float* interleavedAudio, size_t frameCount) {
    // Validate mapping
    const size_t deviceChannelEnd = mapping_.deviceChannelStart + sdp_.numChannels;
    if (deviceChannelEnd > 128) {
        return; // Mapping out of range
    }

    // Stack-allocated temporary buffer for de-interleaving
    constexpr size_t kMaxFrames = 4096;
    if (frameCount > kMaxFrames) {
        return;
    }

    float channelBuffer[kMaxFrames];

    // Write each stream channel to its mapped device channel
    // This de-interleaves: [ch0_f0, ch1_f0, ch0_f1, ch1_f1, ...]
    //                   → deviceChannels[0]: [ch0_f0, ch0_f1, ...]
    //                   → deviceChannels[1]: [ch1_f0, ch1_f1, ...]

    bool hadUnderrun = false;

    for (size_t streamChannel = 0; streamChannel < sdp_.numChannels; ++streamChannel) {
        const size_t deviceChannel = mapping_.deviceChannelStart + streamChannel;

        // Extract this channel from interleaved stream
        for (size_t frame = 0; frame < frameCount; ++frame) {
            channelBuffer[frame] = interleavedAudio[frame * sdp_.numChannels + streamChannel];
        }

        // Write to device ring buffer (batch write)
        const size_t written = deviceChannels_[deviceChannel].write(channelBuffer, frameCount);

        if (written < frameCount && !hadUnderrun) {
            // Ring buffer full - count underrun once per packet
            stats_.underruns.fetch_add(1, std::memory_order_relaxed);
            hadUnderrun = true;
        }
    }
}

void RTPReceiver::updateStats(uint16_t sequenceNumber, size_t payloadSize) {
    // Atomic operations eliminate need for mutex lock

    // Detect packet loss (sequence number gaps)
    // Use signed 16-bit arithmetic so that the two's-complement result
    // correctly distinguishes forward gaps (lost packets) from backward
    // gaps (reordered/duplicate packets), even across 16-bit wraparound.
    uint64_t currentPacketCount = stats_.packetsReceived.load(std::memory_order_relaxed);
    if (currentPacketCount > 0) {
        uint16_t expected = lastSequenceNumber_.load(std::memory_order_relaxed) + 1;
        if (sequenceNumber != expected) {
            int16_t gap = static_cast<int16_t>(sequenceNumber - expected);
            if (gap > 0) {
                // Forward gap: packets between expected and sequenceNumber were lost
                stats_.packetsLost.fetch_add(static_cast<uint64_t>(gap), std::memory_order_relaxed);
            } else {
                // Negative gap: packet arrived out of order (or duplicate)
                stats_.outOfOrderPackets.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    lastSequenceNumber_.store(sequenceNumber, std::memory_order_relaxed);
    stats_.packetsReceived.fetch_add(1, std::memory_order_relaxed);
    stats_.bytesReceived.fetch_add(payloadSize, std::memory_order_relaxed);
}

} // namespace AES67
