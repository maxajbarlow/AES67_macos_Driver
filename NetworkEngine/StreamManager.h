/// @file StreamManager.h
/// @brief Central coordinator for all AES67 RX and TX streams.

#pragma once

#include "../Shared/Types.h"
#include "../Driver/SDPParser.h"
#include "StreamChannelMapper.h"
#include "StreamConfig.h"
#include "RTP/RTPReceiver.h"
#include "RTP/RxContext.h"
#include "RTP/TxContext.h"
#include "Discovery/SAPAnnouncer.h"
#include "RTP/RTPTransmitter.h"
#include "PTP/PTPClock.h"
#include <map>
#include <random>
#include <memory>
#include <mutex>
#include <functional>

namespace AES67 {

/// Central coordinator for all AES67 streams (receivers, transmitters, channel mapping).
///
/// @warning NOT REAL-TIME SAFE. All public methods acquire a mutex. Never call from
/// the Core Audio IO thread or any RT-constrained thread. Safe to call from
/// initialization, UI/manager app, or control threads.
class StreamManager {
public:
    using StreamCallback = std::function<void(const StreamInfo&)>;

    /// Where and how far a TX stream is sent.
    struct TxOptions {
        std::string networkInterface;  // name ("en0") or IP; empty = default route
        uint8_t ttl{32};               // multicast TTL; 0 keeps the stream on this host
    };

    /// @param rxContext Device state receivers place audio against (clock, routing, link offset).
    /// @param txContext Device state transmitters send from (clock, routing the IO thread writes).
    ///
    /// @param sapConfig Where TX streams are announced (SAP, RFC 2974).
    ///
    /// Receivers run only while Core Audio IO is active (setIOActive). Transmitters
    /// run whenever their stream is configured: AES67 senders send continuously,
    /// silence included, and each is announced over SAP while it runs.
    StreamManager(RxContext rxContext, TxContext txContext,
                  SAPAnnouncer::Config sapConfig = SAPAnnouncer::Config{});
    ~StreamManager();

    // Prevent copy/move
    StreamManager(const StreamManager&) = delete;
    StreamManager& operator=(const StreamManager&) = delete;

    //
    // Stream Management - RX
    //

    /// Add an RX stream with automatic channel assignment. Returns the new StreamID.
    StreamID addStream(const SDPSession& sdp);

    /// Add an RX stream with explicit channel mapping.
    StreamID addStream(const SDPSession& sdp, const ChannelMapping& mapping);

    /// Import an RX stream from an SDP file on disk.
    StreamID importSDPFile(const std::string& filepath);

    // Remove stream
    bool removeStream(const StreamID& id);

    // Remove all streams
    void removeAllStreams();

    //
    // Stream Management - TX
    //

    /// Create a TX stream that reads from device output channels and sends RTP.
    StreamID createTxStream(
        const std::string& name,
        const std::string& multicastIP,
        uint16_t port,
        uint16_t numChannels,
        const ChannelMapping& mapping,
        const TxOptions& options
    );

    /// As above with default options (default route, TTL 32). An overload, as
    /// a nested struct's initialisers are not usable in a default argument.
    StreamID createTxStream(
        const std::string& name,
        const std::string& multicastIP,
        uint16_t port,
        uint16_t numChannels,
        const ChannelMapping& mapping
    );

    // Export stream to SDP file
    bool exportSDPFile(const StreamID& id, const std::string& filepath);

    //
    // Channel Mapping
    //

    // Update channel mapping for a stream
    bool updateMapping(const StreamID& id, const ChannelMapping& newMapping);

    // Get mapping for a stream
    std::optional<ChannelMapping> getMapping(const StreamID& id) const;

    // Get all mappings
    std::vector<ChannelMapping> getAllMappings() const;

    //
    // Query
    //

    // Get all active streams
    std::vector<StreamInfo> getActiveStreams() const;

    // Get stream info
    std::optional<StreamInfo> getStreamInfo(const StreamID& id) const;

    // Check if stream exists
    bool hasStream(const StreamID& id) const;

    // Get stream count
    size_t getStreamCount() const;

    //
    // Validation
    //

    // Check if stream can be added
    bool canAddStream(const SDPSession& sdp, std::string* errorOut = nullptr) const;

    // Get detailed error message for why stream can't be added
    std::string getAddStreamError(const SDPSession& sdp) const;

    //
    // Device State
    //

    /// Notify that Core Audio IO has started or stopped.
    /// When active, starts all dormant receivers/transmitters; when inactive, stops them.
    void setIOActive(bool active);

    // Set current device sample rate (validates against streams)
    bool setDeviceSampleRate(double sampleRate);

    /// True if every active stream runs at this rate (always true with no streams).
    bool isSampleRateCompatible(double sampleRate) const;

    // Get current device sample rate
    double getDeviceSampleRate() const { return currentDeviceSampleRate_; }

    // Get available channel count
    size_t getAvailableChannelCount() const;

    //
    // Configuration Persistence
    //

    /// Load saved stream configurations from /tmp/AES67Driver/streams.json.
    bool loadSavedStreams();

    /// Persist all current streams to disk.
    bool saveAllStreams();

    // Enable/disable auto-save (automatically save after add/remove/update)
    void setAutoSave(bool enabled) { autoSaveEnabled_ = enabled; }

    // Get auto-save state
    bool isAutoSaveEnabled() const { return autoSaveEnabled_; }

    //
    // Callbacks
    //

    // Register callback for stream added
    void setStreamAddedCallback(StreamCallback callback) {
        streamAddedCallback_ = callback;
    }

    // Register callback for stream removed
    void setStreamRemovedCallback(StreamCallback callback) {
        streamRemovedCallback_ = callback;
    }

    // Register callback for stream status changed
    void setStreamStatusCallback(StreamCallback callback) {
        streamStatusCallback_ = callback;
    }

private:
    // Internal stream state
    struct ManagedStream {
        SDPSession sdp;
        ChannelMapping mapping;
        std::unique_ptr<RTPReceiver> receiver;
        std::unique_ptr<RTPTransmitter> transmitter;
        StreamInfo info;
        bool isTransmit{false};
        std::string networkInterface;  // as configured (saved with the stream)
    };

    // Validation helpers
    bool validateSampleRate(const SDPSession& sdp, std::string* errorOut) const;
    bool validateChannelAvailability(uint16_t numChannels, std::string* errorOut) const;
    bool validateNetworkConfig(const SDPSession& sdp, std::string* errorOut) const;
    bool streamsSupportSampleRate(double sampleRate) const;  // Caller must hold streamsMutex_

    // Stream creation helpers
    std::unique_ptr<RTPReceiver> createReceiver(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        size_t jitterBufferDepth = 0,
        const std::string& networkInterface = ""
    );

    std::unique_ptr<RTPTransmitter> createTransmitter(
        const SDPSession& sdp,
        const ChannelMapping& mapping,
        const std::string& networkInterface = ""
    );

    // Callback invocation
    void notifyStreamAdded(const StreamInfo& info);
    void notifyStreamRemoved(const StreamInfo& info);
    void notifyStreamStatusChanged(const StreamInfo& info);

    // Announce a running TX stream over SAP (caller holds streamsMutex_)
    void announceTx(const StreamID& id, const ManagedStream& managed);


    // Configuration helpers
    void autoSaveIfEnabled();
    bool saveAllStreamsInternal();  // Internal version without locking

    // Data members
    RxContext rxContext_;                   // RTP receivers place audio here (Network → Core Audio)
    TxContext txContext_;                   // RTP transmitters send from here (Core Audio → Network)
    SAPAnnouncer announcer_;                // announces TX streams while they run
    std::mt19937 sessionIdRandom_{std::random_device{}()};  // guarded by streamsMutex_
    StreamChannelMapper mapper_;
    std::map<StreamID, ManagedStream> streams_;
    mutable std::mutex streamsMutex_;

    // Configuration management
    std::unique_ptr<StreamConfigManager> configManager_;
    bool autoSaveEnabled_{true};

    // IO lifecycle state — true when Core Audio IO is active (StartIO/StopIO)
    std::atomic<bool> ioActive_{false};

    // Device state
    std::atomic<double> currentDeviceSampleRate_{48000.0};

    // PTP clock manager reference
    std::shared_ptr<PTPClockManager> ptpManager_;

    // Callbacks
    StreamCallback streamAddedCallback_;
    StreamCallback streamRemovedCallback_;
    StreamCallback streamStatusCallback_;
};

} // namespace AES67
