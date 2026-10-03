//
// StreamManager.cpp
// AES67 macOS Driver - Build #9
// Unified management of all AES67 streams with validation
//

#include "StreamManager.h"
#include "NetworkUtils.h"
#include "../Driver/DebugLog.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <ctime>

namespace AES67 {

StreamManager::StreamManager(RxContext rxContext, TxContext txContext)
    : rxContext_(rxContext)
    , txContext_(txContext)
    , configManager_(std::make_unique<StreamConfigManager>())
{
}

StreamManager::~StreamManager() {
    removeAllStreams();
}

//
// Stream Management - RX
//

StreamID StreamManager::addStream(const SDPSession& sdp) {
    // Auto-create channel mapping
    auto optMapping = mapper_.createDefaultMapping(sdp);
    if (!optMapping) {
        AES67_LOGF("StreamManager::addStream: failed to create default mapping for '%s' (%u channels)",
                   sdp.sessionName.c_str(), sdp.numChannels);
        return StreamID::null();
    }

    return addStream(sdp, *optMapping);
}

StreamID StreamManager::addStream(const SDPSession& sdp, const ChannelMapping& mapping) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    // Validate stream can be added
    std::string error;
    if (!canAddStream(sdp, &error)) {
        AES67_LOGF("StreamManager::addStream: validation failed for '%s': %s",
                   sdp.sessionName.c_str(), error.c_str());
        return StreamID::null();
    }

    // Generate unique stream ID
    StreamID id = StreamID::generate();

    // Check if stream already exists (shouldn't happen with UUIDs but be safe)
    if (streams_.find(id) != streams_.end()) {
        AES67_LOGF("StreamManager::addStream: duplicate stream ID for '%s'",
                   sdp.sessionName.c_str());
        return StreamID::null();
    }

    // Create complete mapping with stream ID
    ChannelMapping completeMapping = mapping;
    completeMapping.streamID = id;
    completeMapping.streamName = sdp.sessionName;
    completeMapping.streamChannelCount = sdp.numChannels;
    completeMapping.deviceChannelCount = sdp.numChannels;

    // Add mapping to mapper
    if (!mapper_.addMapping(completeMapping)) {
        AES67_LOGF("StreamManager::addStream: mapper rejected mapping for '%s' (devCh=%zu, count=%u)",
                   sdp.sessionName.c_str(), mapping.deviceChannelStart, sdp.numChannels);
        return StreamID::null();
    }

    // Create managed stream
    ManagedStream managed;
    managed.sdp = sdp;
    managed.mapping = completeMapping;
    managed.isTransmit = false;

    // Create RTP receiver
    managed.receiver = createReceiver(sdp, completeMapping);
    if (!managed.receiver) {
        AES67_LOGF("StreamManager::addStream: failed to create RTP receiver for '%s'",
                   sdp.sessionName.c_str());
        mapper_.removeMapping(id);
        return StreamID::null();
    }

    // Only start receiver if IO is active (a Core Audio client has called StartIO).
    // Otherwise the stream is created dormant and will be started by setIOActive(true).
    if (ioActive_.load()) {
        if (!managed.receiver->start()) {
            AES67_LOGF("StreamManager::addStream: failed to start RTP receiver for '%s' (%s:%u)",
                       sdp.sessionName.c_str(), sdp.connectionAddress.c_str(), sdp.port);
            mapper_.removeMapping(id);
            return StreamID::null();
        }
    }

    // Build stream info
    managed.info.id = id;
    managed.info.name = sdp.sessionName;
    managed.info.description = sdp.sessionInfo;

    // Network addresses
    managed.info.source.ip = sdp.sourceAddress;
    managed.info.source.port = sdp.port;
    managed.info.multicast.ip = sdp.connectionAddress;
    managed.info.multicast.port = sdp.port;
    managed.info.multicast.ttl = sdp.ttl;

    // Audio format
    if (sdp.encoding == "L16") {
        managed.info.encoding = AudioEncoding::L16;
    } else if (sdp.encoding == "L24") {
        managed.info.encoding = AudioEncoding::L24;
    } else {
        managed.info.encoding = AudioEncoding::Unknown;
    }

    managed.info.sampleRate = sdp.sampleRate;
    managed.info.numChannels = sdp.numChannels;
    managed.info.payloadType = sdp.payloadType;

    // Timing
    managed.info.ptime = sdp.ptime;
    managed.info.framecount = sdp.framecount;

    // PTP
    managed.info.ptp.domain = sdp.ptpDomain;

    // State
    managed.info.isActive = true;
    managed.info.isConnected = false;
    managed.info.startTime = std::chrono::steady_clock::now();

    // Store stream
    streams_[id] = std::move(managed);

    // Notify callback
    notifyStreamAdded(streams_[id].info);

    // Auto-save configuration
    autoSaveIfEnabled();

    return id;
}

StreamID StreamManager::importSDPFile(const std::string& filepath) {
    // Read SDP file
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return StreamID::null();
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string sdpContent = buffer.str();

    // Parse SDP
    auto sdpSession = SDPParser::parseString(sdpContent);
    if (!sdpSession) {
        return StreamID::null();
    }

    // Add stream with auto-mapping
    return addStream(*sdpSession);
}

bool StreamManager::removeStream(const StreamID& id) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(id);
    if (it == streams_.end()) {
        return false;
    }

    // Save info for callback before deletion
    StreamInfo info = it->second.info;

    // Stop receiver/transmitter
    if (it->second.receiver) {
        it->second.receiver->stop();
    }
    if (it->second.transmitter) {
        it->second.transmitter->stop();
    }

    // Remove from mapper
    mapper_.removeMapping(id);

    // Remove from map
    streams_.erase(it);

    // Notify callback
    notifyStreamRemoved(info);

    // Auto-save configuration
    autoSaveIfEnabled();

    return true;
}

void StreamManager::removeAllStreams() {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    // Stop all streams
    for (auto& pair : streams_) {
        if (pair.second.receiver) {
            pair.second.receiver->stop();
        }
        if (pair.second.transmitter) {
            pair.second.transmitter->stop();
        }

        notifyStreamRemoved(pair.second.info);
    }

    streams_.clear();
    mapper_.clearAll();
}

//
// Stream Management - TX
//

StreamID StreamManager::createTxStream(
    const std::string& name,
    const std::string& multicastIP,
    uint16_t port,
    uint16_t numChannels,
    const ChannelMapping& mapping
) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    // Build SDP session for transmit stream
    SDPSession sdp;
    sdp.sessionName = name;
    sdp.connectionAddress = multicastIP;
    sdp.port = port;
    sdp.numChannels = numChannels;
    sdp.sampleRate = currentDeviceSampleRate_.load();
    sdp.encoding = "L24"; // Use L24 for best quality
    sdp.payloadType = 97; // Dynamic payload type
    sdp.direction = "sendonly"; // loadSavedStreams() relies on this to recreate a transmitter
    sdp.sessionID = static_cast<uint64_t>(std::time(nullptr));
    sdp.sessionVersion = 1;

    // Validate
    std::string error;
    if (!canAddStream(sdp, &error)) {
        AES67_LOGF("StreamManager::createTxStream: validation failed for '%s': %s",
                   name.c_str(), error.c_str());
        return StreamID::null();
    }

    // Generate stream ID
    StreamID id = StreamID::generate();

    // Create complete mapping with stream ID
    ChannelMapping completeMapping = mapping;
    completeMapping.streamID = id;
    completeMapping.streamName = name;
    completeMapping.streamChannelCount = numChannels;
    completeMapping.deviceChannelCount = numChannels;

    // Add mapping
    if (!mapper_.addMapping(completeMapping)) {
        AES67_LOGF("StreamManager::createTxStream: mapper rejected mapping for '%s' (devCh=%zu, count=%u)",
                   name.c_str(), mapping.deviceChannelStart, numChannels);
        return StreamID::null();
    }

    // Create managed stream
    ManagedStream managed;
    managed.sdp = sdp;
    managed.mapping = completeMapping;
    managed.isTransmit = true;

    // Create RTP transmitter
    managed.transmitter = createTransmitter(sdp, completeMapping);
    if (!managed.transmitter) {
        AES67_LOGF("StreamManager::createTxStream: failed to create RTP transmitter for '%s'",
                   name.c_str());
        mapper_.removeMapping(id);
        return StreamID::null();
    }

    // Transmit continuously from now on, whether or not Core Audio IO runs
    if (!managed.transmitter->start()) {
        AES67_LOGF("StreamManager::createTxStream: failed to start RTP transmitter for '%s' (%s:%u)",
                   name.c_str(), multicastIP.c_str(), port);
        mapper_.removeMapping(id);
        return StreamID::null();
    }

    // Build stream info
    managed.info.id = id;
    managed.info.name = name;
    managed.info.multicast.ip = multicastIP;
    managed.info.multicast.port = port;
    managed.info.encoding = AudioEncoding::L24;
    managed.info.sampleRate = sdp.sampleRate;
    managed.info.numChannels = numChannels;
    managed.info.payloadType = sdp.payloadType;
    managed.info.isActive = true;
    managed.info.startTime = std::chrono::steady_clock::now();

    // Store stream
    streams_[id] = std::move(managed);

    // Notify callback
    notifyStreamAdded(streams_[id].info);

    // Auto-save configuration
    autoSaveIfEnabled();

    return id;
}

bool StreamManager::exportSDPFile(const StreamID& id, const std::string& filepath) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(id);
    if (it == streams_.end()) {
        return false;
    }

    // Generate SDP content
    std::string sdpContent = SDPParser::generate(it->second.sdp);
    if (sdpContent.empty()) {
        return false;
    }

    // Write to file
    std::ofstream file(filepath);
    if (!file.is_open()) {
        return false;
    }

    file << sdpContent;
    return true;
}

//
// Channel Mapping
//

bool StreamManager::updateMapping(const StreamID& id, const ChannelMapping& newMapping) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(id);
    if (it == streams_.end()) {
        return false;
    }

    // Update mapper
    ChannelMapping completeMapping = newMapping;
    completeMapping.streamID = id;
    completeMapping.streamName = it->second.mapping.streamName;
    completeMapping.streamChannelCount = it->second.sdp.numChannels;

    if (!mapper_.updateMapping(completeMapping)) {
        return false;
    }

    // Update managed stream
    it->second.mapping = completeMapping;

    // Update receiver/transmitter
    bool updated = false;
    if (it->second.receiver) {
        updated = it->second.receiver->updateMapping(completeMapping);
    } else if (it->second.transmitter) {
        updated = it->second.transmitter->updateMapping(completeMapping);
    }

    if (updated) {
        notifyStreamStatusChanged(it->second.info);

        // Auto-save configuration
        autoSaveIfEnabled();
    }

    return updated;
}

std::optional<ChannelMapping> StreamManager::getMapping(const StreamID& id) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return mapper_.getMapping(id);
}

std::vector<ChannelMapping> StreamManager::getAllMappings() const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return mapper_.getAllMappings();
}

//
// Query
//

std::vector<StreamInfo> StreamManager::getActiveStreams() const {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    std::vector<StreamInfo> activeStreams;
    for (const auto& pair : streams_) {
        if (pair.second.info.isActive) {
            activeStreams.push_back(pair.second.info);
        }
    }

    return activeStreams;
}

std::optional<StreamInfo> StreamManager::getStreamInfo(const StreamID& id) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(id);
    if (it != streams_.end()) {
        return it->second.info;
    }

    return std::nullopt;
}

bool StreamManager::hasStream(const StreamID& id) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return streams_.find(id) != streams_.end();
}

size_t StreamManager::getStreamCount() const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return streams_.size();
}

//
// Validation
//

bool StreamManager::canAddStream(const SDPSession& sdp, std::string* errorOut) const {
    if (!validateSampleRate(sdp, errorOut)) {
        return false;
    }

    if (!validateChannelAvailability(sdp.numChannels, errorOut)) {
        return false;
    }

    if (!validateNetworkConfig(sdp, errorOut)) {
        return false;
    }

    return true;
}

std::string StreamManager::getAddStreamError(const SDPSession& sdp) const {
    std::string error;
    canAddStream(sdp, &error);
    return error;
}

//
// Device State
//

void StreamManager::setIOActive(bool active) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    bool wasActive = ioActive_.exchange(active);
    if (wasActive == active) {
        return; // No state change
    }

    // Receivers follow Core Audio IO; transmitters run whenever configured
    if (active) {
        AES67_LOGF("StreamManager::setIOActive: Starting receivers (%zu stream(s))", streams_.size());
        for (auto& [id, managed] : streams_) {
            if (managed.receiver) {
                managed.receiver->start();
            }
        }
    } else {
        AES67_LOGF("StreamManager::setIOActive: Stopping receivers (%zu stream(s))", streams_.size());
        for (auto& [id, managed] : streams_) {
            if (managed.receiver) {
                managed.receiver->stop();
            }
        }
    }
}

bool StreamManager::setDeviceSampleRate(double sampleRate) {
    if (sampleRate < 44100 || sampleRate > 384000) {
        return false;
    }

    std::lock_guard<std::mutex> lock(streamsMutex_);

    if (!streamsSupportSampleRate(sampleRate)) {
        return false;
    }

    currentDeviceSampleRate_.store(sampleRate);
    return true;
}

bool StreamManager::isSampleRateCompatible(double sampleRate) const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return streamsSupportSampleRate(sampleRate);
}

bool StreamManager::streamsSupportSampleRate(double sampleRate) const {
    for (const auto& pair : streams_) {
        if (std::abs(pair.second.sdp.sampleRate - sampleRate) > 0.1) {
            return false;
        }
    }
    return true;
}

size_t StreamManager::getAvailableChannelCount() const {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return mapper_.getAvailableChannelCount();
}

//
// Validation Helpers
//

bool StreamManager::validateSampleRate(const SDPSession& sdp, std::string* errorOut) const {
    const double deviceRate = currentDeviceSampleRate_.load();

    if (std::abs(sdp.sampleRate - deviceRate) > 0.1) {
        if (errorOut) {
            *errorOut = "Sample rate mismatch: stream=" + std::to_string(static_cast<int>(sdp.sampleRate)) +
                       " Hz, device=" + std::to_string(static_cast<int>(deviceRate)) + " Hz";
        }
        return false;
    }

    return true;
}

bool StreamManager::validateChannelAvailability(uint16_t numChannels, std::string* errorOut) const {
    if (numChannels == 0 || numChannels > 128) {
        if (errorOut) {
            *errorOut = "Invalid channel count: " + std::to_string(numChannels) + " (must be 1-128)";
        }
        return false;
    }

    size_t available = mapper_.getAvailableChannelCount();
    if (numChannels > available) {
        if (errorOut) {
            *errorOut = "Insufficient channels: need " + std::to_string(numChannels) +
                       ", have " + std::to_string(available);
        }
        return false;
    }

    return true;
}

bool StreamManager::validateNetworkConfig(const SDPSession& sdp, std::string* errorOut) const {
    if (sdp.connectionAddress.empty()) {
        if (errorOut) {
            *errorOut = "Missing multicast IP address";
        }
        return false;
    }

    // Check for valid multicast range (239.x.x.x for AES67)
    if (sdp.connectionAddress.substr(0, 4) != "239.") {
        if (errorOut) {
            *errorOut = "Invalid multicast IP: " + sdp.connectionAddress +
                       " (AES67 requires 239.x.x.x)";
        }
        return false;
    }

    if (sdp.port == 0) {
        if (errorOut) {
            *errorOut = "Invalid port: 0";
        }
        return false;
    }

    // Check multicast routing configuration
    // Get primary ethernet interface for the check
    std::string primaryIface = NetworkUtils::getPrimaryEthernetInterface();

    if (!NetworkUtils::hasMulticastRoute(primaryIface)) {
        if (errorOut) {
            std::string iface = primaryIface.empty() ? "en0" : primaryIface;
            *errorOut = "WARNING: Multicast routing not configured. Audio may not flow.\n"
                       "To fix, run: " + NetworkUtils::getMulticastRouteCommand(iface) + "\n"
                       "Stream will be added but may not receive traffic.";
        }
        // Log warning but don't block stream addition
        AES67_LOG("WARNING: Multicast routing not configured - audio may not flow");
        if (!primaryIface.empty()) {
            AES67_LOGF("Run: %s", NetworkUtils::getMulticastRouteCommand(primaryIface).c_str());
        }
        // Return true to allow stream addition with warning
    }

    return true;
}

//
// Stream Creation Helpers
//

std::unique_ptr<RTPReceiver> StreamManager::createReceiver(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    size_t jitterBufferDepth,
    const std::string& networkInterface
) {
    // Receivers place decoded network audio by timestamp (Network → Core Audio).
    // jitterBufferDepth is a legacy setting: the playout buffer is sized from
    // the link offset instead.
    (void)jitterBufferDepth;
    return std::make_unique<RTPReceiver>(sdp, mapping, rxContext_, networkInterface);
}

std::unique_ptr<RTPTransmitter> StreamManager::createTransmitter(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    const std::string& networkInterface
) {
    // Transmitters read audio from OUTPUT buffers (Core Audio → Network)
    return std::make_unique<RTPTransmitter>(sdp, mapping, txContext_, networkInterface);
}

//
// Callback Invocation
//

void StreamManager::notifyStreamAdded(const StreamInfo& info) {
    if (streamAddedCallback_) {
        streamAddedCallback_(info);
    }
}

void StreamManager::notifyStreamRemoved(const StreamInfo& info) {
    if (streamRemovedCallback_) {
        streamRemovedCallback_(info);
    }
}

void StreamManager::notifyStreamStatusChanged(const StreamInfo& info) {
    if (streamStatusCallback_) {
        streamStatusCallback_(info);
    }
}

//
// Configuration Persistence
//

bool StreamManager::loadSavedStreams() {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    // Load configurations from disk
    auto configs = configManager_->loadConfig();
    if (!configs) {
        AES67_LOG("StreamManager: No saved stream configurations found");
        return false;
    }

    AES67_LOGF("StreamManager: Loading %zu saved stream configurations", configs->size());

    // Add each saved stream
    int loadedCount = 0;
    int failedCount = 0;

    for (const auto& config : *configs) {
        // Skip disabled streams
        if (!config.enabled) {
            AES67_LOGF("StreamManager: Skipping disabled stream: %s", config.sdp.sessionName.c_str());
            continue;
        }

        // Validate config
        if (!config.isValid()) {
            AES67_LOGF("StreamManager: Invalid stream config: %s", config.sdp.sessionName.c_str());
            failedCount++;
            continue;
        }

        // Check if stream can be added (validate sample rate, channels, etc.)
        std::string error;
        if (!canAddStream(config.sdp, &error)) {
            AES67_LOGF("StreamManager: Cannot add stream '%s': %s",
                      config.sdp.sessionName.c_str(), error.c_str());
            failedCount++;
            continue;
        }

        // Generate new StreamID (use the one from saved mapping)
        StreamID id = config.mapping.streamID;

        // Check if stream already exists
        if (streams_.find(id) != streams_.end()) {
            AES67_LOGF("StreamManager: Stream already exists: %s", config.sdp.sessionName.c_str());
            failedCount++;
            continue;
        }

        // Add mapping to mapper
        if (!mapper_.addMapping(config.mapping)) {
            AES67_LOGF("StreamManager: Failed to add mapping for stream: %s",
                      config.sdp.sessionName.c_str());
            failedCount++;
            continue;
        }

        // Create managed stream
        ManagedStream managed;
        managed.sdp = config.sdp;
        managed.mapping = config.mapping;
        managed.isTransmit = (config.sdp.direction == "sendonly" || config.sdp.direction == "sendrecv");

        // Transmitters start now; receivers start only if IO is active
        if (managed.isTransmit) {
            managed.transmitter = createTransmitter(config.sdp, config.mapping, config.networkInterface);
            if (!managed.transmitter) {
                mapper_.removeMapping(id);
                failedCount++;
                continue;
            }
            if (!managed.transmitter->start()) {
                mapper_.removeMapping(id);
                failedCount++;
                continue;
            }
        } else {
            managed.receiver = createReceiver(config.sdp, config.mapping, config.jitterBufferDepth, config.networkInterface);
            if (!managed.receiver) {
                mapper_.removeMapping(id);
                failedCount++;
                continue;
            }
            if (ioActive_.load() && !managed.receiver->start()) {
                mapper_.removeMapping(id);
                failedCount++;
                continue;
            }
        }

        // Build stream info (same as in addStream)
        managed.info.id = id;
        managed.info.name = config.sdp.sessionName;
        managed.info.description = config.sdp.sessionInfo;
        managed.info.source.ip = config.sdp.sourceAddress;
        managed.info.source.port = config.sdp.port;
        managed.info.multicast.ip = config.sdp.connectionAddress;
        managed.info.multicast.port = config.sdp.port;
        managed.info.multicast.ttl = config.sdp.ttl;

        if (config.sdp.encoding == "L16") {
            managed.info.encoding = AudioEncoding::L16;
        } else if (config.sdp.encoding == "L24") {
            managed.info.encoding = AudioEncoding::L24;
        } else {
            managed.info.encoding = AudioEncoding::Unknown;
        }

        managed.info.sampleRate = config.sdp.sampleRate;
        managed.info.numChannels = config.sdp.numChannels;
        managed.info.payloadType = config.sdp.payloadType;
        managed.info.ptime = config.sdp.ptime;
        managed.info.framecount = config.sdp.framecount;
        managed.info.ptp.domain = config.sdp.ptpDomain;
        managed.info.isActive = true;
        managed.info.isConnected = false;
        managed.info.startTime = std::chrono::steady_clock::now();

        // Store stream
        streams_[id] = std::move(managed);

        // Notify callback
        notifyStreamAdded(streams_[id].info);

        loadedCount++;
        AES67_LOGF("StreamManager: Loaded stream: %s (%s)",
                  config.sdp.sessionName.c_str(),
                  id.toString().c_str());
    }

    AES67_LOGF("StreamManager: Loaded %d streams successfully, %d failed",
              loadedCount, failedCount);

    return loadedCount > 0;
}

bool StreamManager::saveAllStreams() {
    std::lock_guard<std::mutex> lock(streamsMutex_);
    return saveAllStreamsInternal();
}

bool StreamManager::saveAllStreamsInternal() {
    // NOTE: Caller must hold streamsMutex_ lock

    std::vector<PersistedStreamConfig> configs;
    configs.reserve(streams_.size());

    // Convert all streams to persisted configs
    for (const auto& pair : streams_) {
        const auto& managed = pair.second;

        PersistedStreamConfig config = StreamConfigManager::createConfig(
            managed.sdp,
            managed.mapping,
            managed.info.description
        );

        configs.push_back(config);
    }

    // Save to disk
    bool success = configManager_->saveConfig(configs);

    if (success) {
        AES67_LOGF("StreamManager: Saved %zu stream configurations to disk", configs.size());
    } else {
        AES67_LOG("StreamManager: Failed to save stream configurations");
    }

    return success;
}

void StreamManager::autoSaveIfEnabled() {
    // NOTE: We're already holding streamsMutex_ when this is called
    // from addStream/removeStream/updateMapping, so use the internal version
    if (autoSaveEnabled_) {
        saveAllStreamsInternal();
    }
}

} // namespace AES67
