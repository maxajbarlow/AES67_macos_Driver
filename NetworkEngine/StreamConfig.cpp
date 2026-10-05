//
// StreamConfig.cpp
// AES67 macOS Driver - Build #10
// Stream configuration persistence implementation
//

#include "StreamConfig.h"
#include "NetworkUtils.h"
#include "../Driver/DebugLog.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <sys/stat.h>
#include <ctime>
#include <regex>
#include <cstdlib>
#include <pwd.h>
#include <unistd.h>

namespace AES67 {

// ============================================================================
// PersistedStreamConfig Implementation
// ============================================================================

bool PersistedStreamConfig::isValid() const {
    bool sdpValid = sdp.isValid();
    bool mappingValid = mapping.isValid();

    if (!sdpValid) {
        AES67_LOG("PersistedStreamConfig: SDP is invalid");
    }
    if (!mappingValid) {
        AES67_LOG("PersistedStreamConfig: Mapping is invalid");
    }

    return sdpValid && mappingValid;
}

// ============================================================================
// StreamConfigManager Implementation
// ============================================================================

StreamConfigManager::StreamConfigManager() {
    // Search for existing config in priority order, or use default
    std::string existingConfig = findExistingConfig();
    if (!existingConfig.empty()) {
        configPath_ = existingConfig;
        AES67_LOGF("StreamConfigManager: Found config at: %s", configPath_.c_str());
    } else {
        // Default to system-wide location for new configs
        configPath_ = "/Library/Application Support/AES67Driver/" + defaultConfigFile_;
        AES67_LOGF("StreamConfigManager: No existing config found, will use: %s", configPath_.c_str());
    }

    // Log available interfaces for debugging
    auto interfaces = NetworkUtils::getActiveInterfacesWithIPs();
    AES67_LOGF("StreamConfigManager: Available network interfaces (%zu):", interfaces.size());
    for (const auto& iface : interfaces) {
        AES67_LOGF("  - %s: %s", iface.first.c_str(), iface.second.c_str());
    }
}

std::vector<std::string> StreamConfigManager::getConfigSearchPaths() {
    std::vector<std::string> paths;

    // 1. Environment variable override (highest priority)
    const char* envPath = std::getenv("AES67_CONFIG_PATH");
    if (envPath && envPath[0] != '\0') {
        paths.push_back(envPath);
        AES67_LOGF("StreamConfigManager: AES67_CONFIG_PATH set to: %s", envPath);
    }

    // 2. User-level config (~/.config/AES67Driver or ~/Library/Application Support)
    const char* home = std::getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (home && home[0] != '\0') {
        std::string userPath = std::string(home) + "/Library/Application Support/AES67Driver/streams.json";
        paths.push_back(userPath);
    }

    // 3. System-wide config (default)
    paths.push_back("/Library/Application Support/AES67Driver/streams.json");

    return paths;
}

std::string StreamConfigManager::findExistingConfig() {
    auto searchPaths = getConfigSearchPaths();

    for (const auto& path : searchPaths) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            AES67_LOGF("StreamConfigManager: Found config file at: %s", path.c_str());
            return path;
        }
    }

    return "";
}

StreamConfigManager::~StreamConfigManager() = default;

std::string StreamConfigManager::getConfigPath() const {
    return configPath_;
}

void StreamConfigManager::setConfigPath(const std::string& path) {
    configPath_ = path;
}

bool StreamConfigManager::ensureConfigDirectoryExists() {
    // Extract directory from config path
    size_t lastSlash = configPath_.find_last_of('/');
    if (lastSlash == std::string::npos) {
        return false;
    }

    std::string dir = configPath_.substr(0, lastSlash);

    // Check if directory already exists
    struct stat st;
    if (stat(dir.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    // Directory doesn't exist — create parent first, then target.
    // For /Library/Application Support/AES67Driver/ the parent should already exist,
    // but handle the case where it doesn't gracefully.
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        AES67_LOGF("StreamConfigManager: Failed to create directory '%s': %s",
                   dir.c_str(), ec.message().c_str());
        return false;
    }

    // Set directory permissions to 755 (rwxr-xr-x)
    chmod(dir.c_str(), 0755);

    return true;
}

bool StreamConfigManager::saveConfig(const std::vector<PersistedStreamConfig>& configs) {
    if (!ensureConfigDirectoryExists()) {
        AES67_LOG("StreamConfigManager: Failed to create config directory");
        return false;
    }

    std::string json = toJSON(configs);
    if (json.empty()) {
        AES67_LOG("StreamConfigManager: Failed to serialize configs to JSON");
        return false;
    }

    std::ofstream file(configPath_);
    if (!file.is_open()) {
        AES67_LOGF("StreamConfigManager: Failed to open config file for writing: %s", configPath_.c_str());
        return false;
    }

    file << json;
    AES67_LOGF("StreamConfigManager: Saved %zu stream configurations to %s", configs.size(), configPath_.c_str());
    return true;
}

std::optional<std::vector<PersistedStreamConfig>> StreamConfigManager::loadConfig() {
    std::ifstream file(configPath_);
    if (!file.is_open()) {
        AES67_LOGF("StreamConfigManager: Config file not found: %s", configPath_.c_str());
        return std::nullopt;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json = buffer.str();
    file.close();

    auto configs = fromJSON(json);
    if (configs) {
        AES67_LOGF("StreamConfigManager: Loaded %zu stream configurations from %s", configs->size(), configPath_.c_str());
    } else {
        // JSON is corrupt — back it up and start fresh
        AES67_LOGF("WARNING: StreamConfigManager: Config file is corrupt, backing up to %s.bak",
                   configPath_.c_str());

        std::string backupPath = configPath_ + ".bak";
        std::error_code ec;
        std::filesystem::copy_file(configPath_, backupPath,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            AES67_LOGF("StreamConfigManager: Failed to create backup: %s", ec.message().c_str());
        }

        // Remove the corrupt file so a fresh one can be written on next save
        std::filesystem::remove(configPath_, ec);
    }

    return configs;
}

// ============================================================================
// JSON Serialization
// ============================================================================

std::string StreamConfigManager::toJSON(const std::vector<PersistedStreamConfig>& configs) {
    std::ostringstream json;
    json << "{\n";
    json << "  \"version\": \"1.0\",\n";
    json << "  \"streams\": [\n";

    bool first = true;
    for (const auto& config : configs) {
        if (!first) json << ",\n";
        first = false;

        json << "    " << configToJSON(config);
    }

    json << "\n  ]\n";
    json << "}";

    return json.str();
}

std::string StreamConfigManager::configToJSON(const PersistedStreamConfig& config) {
    std::ostringstream json;
    json << "{\n";
    json << "      \"enabled\": " << (config.enabled ? "true" : "false") << ",\n";
    json << "      \"description\": \"" << escapeJSON(config.description) << "\",\n";
    json << "      \"createdTimestamp\": " << config.createdTimestamp << ",\n";
    json << "      \"modifiedTimestamp\": " << config.modifiedTimestamp << ",\n";
    json << "      \"jitterBufferDepth\": " << config.jitterBufferDepth << ",\n";
    json << "      \"networkInterface\": \"" << escapeJSON(config.networkInterface) << "\",\n";
    json << "      \"sdp\": " << sdpToJSON(config.sdp) << ",\n";
    json << "      \"mapping\": " << mappingToJSON(config.mapping) << "\n";
    json << "    }";

    return json.str();
}

std::string StreamConfigManager::sdpToJSON(const SDPSession& sdp) {
    std::ostringstream json;
    json << "{\n";
    json << "        \"sessionName\": \"" << escapeJSON(sdp.sessionName) << "\",\n";
    json << "        \"sessionInfo\": \"" << escapeJSON(sdp.sessionInfo) << "\",\n";
    json << "        \"sessionID\": " << sdp.sessionID << ",\n";
    json << "        \"sessionVersion\": " << sdp.sessionVersion << ",\n";
    json << "        \"originUsername\": \"" << escapeJSON(sdp.originUsername) << "\",\n";
    json << "        \"originAddress\": \"" << escapeJSON(sdp.originAddress) << "\",\n";
    json << "        \"connectionAddress\": \"" << escapeJSON(sdp.connectionAddress) << "\",\n";
    json << "        \"ttl\": " << static_cast<int>(sdp.ttl) << ",\n";
    json << "        \"port\": " << sdp.port << ",\n";
    json << "        \"payloadType\": " << static_cast<int>(sdp.payloadType) << ",\n";
    json << "        \"encoding\": \"" << escapeJSON(sdp.encoding) << "\",\n";
    json << "        \"sampleRate\": " << sdp.sampleRate << ",\n";
    json << "        \"numChannels\": " << sdp.numChannels << ",\n";
    json << "        \"ptime\": " << sdp.ptime << ",\n";
    json << "        \"framecount\": " << sdp.framecount << ",\n";
    json << "        \"sourceAddress\": \"" << escapeJSON(sdp.sourceAddress) << "\",\n";
    json << "        \"ptpDomain\": " << sdp.ptpDomain << ",\n";
    json << "        \"ptpMasterMAC\": \"" << escapeJSON(sdp.ptpMasterMAC) << "\",\n";
    json << "        \"mediaClockType\": \"" << escapeJSON(sdp.mediaClockType) << "\",\n";
    json << "        \"direction\": \"" << escapeJSON(sdp.direction) << "\"\n";
    json << "      }";

    return json.str();
}

std::string StreamConfigManager::mappingToJSON(const ChannelMapping& mapping) {
    std::ostringstream json;
    json << "{\n";
    json << "        \"streamID\": \"" << mapping.streamID.toString() << "\",\n";
    json << "        \"streamName\": \"" << escapeJSON(mapping.streamName) << "\",\n";
    json << "        \"streamChannelCount\": " << mapping.streamChannelCount << ",\n";
    json << "        \"streamChannelOffset\": " << mapping.streamChannelOffset << ",\n";
    json << "        \"deviceChannelStart\": " << mapping.deviceChannelStart << ",\n";
    json << "        \"deviceChannelCount\": " << mapping.deviceChannelCount << ",\n";
    json << "        \"channelMap\": [";

    for (size_t i = 0; i < mapping.channelMap.size(); i++) {
        if (i > 0) json << ", ";
        json << mapping.channelMap[i];
    }

    json << "]\n";
    json << "      }";

    return json.str();
}

// ============================================================================
// JSON Deserialization
// ============================================================================

std::optional<std::vector<PersistedStreamConfig>> StreamConfigManager::fromJSON(const std::string& json) {
    std::vector<PersistedStreamConfig> configs;

    // Find streams array manually (regex with . doesn't match newlines by default)
    size_t arrayStart = json.find("\"streams\"");
    if (arrayStart == std::string::npos) {
        AES67_LOG("StreamConfigManager: Failed to find streams field in JSON");
        return std::nullopt;
    }

    size_t bracketStart = json.find('[', arrayStart);
    if (bracketStart == std::string::npos) {
        AES67_LOG("StreamConfigManager: Failed to find streams array opening bracket");
        return std::nullopt;
    }

    // Find matching closing bracket
    int bracketDepth = 0;
    size_t bracketEnd = std::string::npos;
    for (size_t i = bracketStart; i < json.length(); i++) {
        if (json[i] == '[') bracketDepth++;
        else if (json[i] == ']') {
            bracketDepth--;
            if (bracketDepth == 0) {
                bracketEnd = i;
                break;
            }
        }
    }

    if (bracketEnd == std::string::npos) {
        AES67_LOG("StreamConfigManager: Failed to find streams array closing bracket");
        return std::nullopt;
    }

    std::string streamsContent = json.substr(bracketStart + 1, bracketEnd - bracketStart - 1);

    // Split stream objects (basic split on }{ pattern)
    std::vector<std::string> streamObjects;
    int braceDepth = 0;
    std::string currentObject;

    for (size_t i = 0; i < streamsContent.length(); i++) {
        char c = streamsContent[i];
        currentObject += c;

        if (c == '{') {
            braceDepth++;
        } else if (c == '}') {
            braceDepth--;
            if (braceDepth == 0 && !currentObject.empty()) {
                streamObjects.push_back(currentObject);
                currentObject.clear();
            }
        }
    }

    // Parse each stream object
    for (const auto& obj : streamObjects) {
        auto config = configFromJSON(obj);
        if (config) {
            configs.push_back(*config);
        }
    }

    return configs;
}

std::optional<PersistedStreamConfig> StreamConfigManager::configFromJSON(const std::string& json) {
    PersistedStreamConfig config;

    // Parse top-level fields
    if (auto enabled = extractBoolField(json, "enabled")) {
        config.enabled = *enabled;
    }

    if (auto desc = extractStringField(json, "description")) {
        config.description = *desc;
    }

    if (auto created = extractUInt64Field(json, "createdTimestamp")) {
        config.createdTimestamp = *created;
    }

    if (auto modified = extractUInt64Field(json, "modifiedTimestamp")) {
        config.modifiedTimestamp = *modified;
    }

    if (auto jbDepth = extractUInt64Field(json, "jitterBufferDepth")) {
        config.jitterBufferDepth = static_cast<size_t>(*jbDepth);
    }

    if (auto iface = extractStringField(json, "networkInterface")) {
        // Kept as written ("en0", an IPv4 address, or "auto"/""): streams
        // resolve it each time they start. Resolving it here froze the
        // address of the moment, and saving then made it permanent.
        config.networkInterface = *iface;
    }
    // No interface specified: left empty, which means the primary interface
    // as it is when the stream starts

    // Extract SDP object
    std::regex sdpRegex(R"("sdp"\s*:\s*\{([^}]+(?:\{[^}]+\})?[^}]*)\})");
    std::smatch sdpMatch;
    if (std::regex_search(json, sdpMatch, sdpRegex)) {
        std::string sdpJson = "{" + sdpMatch[1].str() + "}";
        auto sdp = sdpFromJSON(sdpJson);
        if (sdp) {
            config.sdp = *sdp;
        } else {
            AES67_LOG("StreamConfigManager: Failed to parse SDP from JSON");
            return std::nullopt;
        }
    }

    // Extract mapping object
    std::regex mappingRegex(R"("mapping"\s*:\s*\{([^}]+(?:\{[^}]+\})?[^}]*)\})");
    std::smatch mappingMatch;
    if (std::regex_search(json, mappingMatch, mappingRegex)) {
        std::string mappingJson = "{" + mappingMatch[1].str() + "}";
        auto mapping = mappingFromJSON(mappingJson);
        if (mapping) {
            config.mapping = *mapping;
        } else {
            AES67_LOG("StreamConfigManager: Failed to parse mapping from JSON");
            return std::nullopt;
        }
    }

    // Validate
    if (!config.isValid()) {
        AES67_LOG("StreamConfigManager: Parsed config is invalid");
        return std::nullopt;
    }

    return config;
}

std::optional<SDPSession> StreamConfigManager::sdpFromJSON(const std::string& json) {
    SDPSession sdp;

    if (auto val = extractStringField(json, "sessionName")) sdp.sessionName = *val;
    if (auto val = extractStringField(json, "sessionInfo")) sdp.sessionInfo = *val;
    if (auto val = extractUInt64Field(json, "sessionID")) sdp.sessionID = *val;
    if (auto val = extractUInt64Field(json, "sessionVersion")) sdp.sessionVersion = *val;
    if (auto val = extractStringField(json, "originUsername")) sdp.originUsername = *val;
    if (auto val = extractStringField(json, "originAddress")) sdp.originAddress = *val;
    if (auto val = extractStringField(json, "connectionAddress")) sdp.connectionAddress = *val;
    if (auto val = extractUInt8Field(json, "ttl")) sdp.ttl = *val;
    if (auto val = extractUInt16Field(json, "port")) sdp.port = *val;
    if (auto val = extractUInt8Field(json, "payloadType")) sdp.payloadType = *val;
    if (auto val = extractStringField(json, "encoding")) sdp.encoding = *val;
    if (auto val = extractUInt32Field(json, "sampleRate")) sdp.sampleRate = static_cast<double>(*val);
    if (auto val = extractUInt16Field(json, "numChannels")) sdp.numChannels = *val;
    if (auto val = extractDoubleField(json, "ptime")) sdp.ptime = *val;  // fractional ms allowed
    if (auto val = extractUInt32Field(json, "framecount")) sdp.framecount = *val;
    if (auto val = extractStringField(json, "sourceAddress")) sdp.sourceAddress = *val;
    if (auto val = extractIntField(json, "ptpDomain")) sdp.ptpDomain = *val;
    if (auto val = extractStringField(json, "ptpMasterMAC")) sdp.ptpMasterMAC = *val;
    if (auto val = extractStringField(json, "mediaClockType")) sdp.mediaClockType = *val;
    if (auto val = extractStringField(json, "direction")) sdp.direction = *val;

    return sdp;
}

std::optional<ChannelMapping> StreamConfigManager::mappingFromJSON(const std::string& json) {
    ChannelMapping mapping;

    if (auto val = extractStringField(json, "streamID")) {
        mapping.streamID = StreamID(*val);
    }
    if (auto val = extractStringField(json, "streamName")) mapping.streamName = *val;
    if (auto val = extractUInt16Field(json, "streamChannelCount")) mapping.streamChannelCount = *val;
    if (auto val = extractUInt16Field(json, "streamChannelOffset")) mapping.streamChannelOffset = *val;
    if (auto val = extractUInt16Field(json, "deviceChannelStart")) mapping.deviceChannelStart = *val;
    if (auto val = extractUInt16Field(json, "deviceChannelCount")) mapping.deviceChannelCount = *val;

    // Parse channelMap array
    std::regex arrayRegex(R"("channelMap"\s*:\s*\[([^\]]*)\])");
    std::smatch arrayMatch;
    if (std::regex_search(json, arrayMatch, arrayRegex)) {
        std::string arrayContent = arrayMatch[1].str();
        std::regex numberRegex(R"((-?\d+))");
        std::sregex_iterator it(arrayContent.begin(), arrayContent.end(), numberRegex);
        std::sregex_iterator end;

        while (it != end) {
            int value = std::stoi((*it)[1].str());
            mapping.channelMap.push_back(value);
            ++it;
        }
    }

    return mapping;
}

// ============================================================================
// Helper Functions
// ============================================================================

PersistedStreamConfig StreamConfigManager::createConfig(
    const SDPSession& sdp,
    const ChannelMapping& mapping,
    const std::string& description
) {
    PersistedStreamConfig config;
    config.sdp = sdp;
    config.mapping = mapping;
    config.description = description;
    config.enabled = true;
    config.createdTimestamp = getCurrentTimestamp();
    config.modifiedTimestamp = config.createdTimestamp;

    return config;
}

uint64_t StreamConfigManager::getCurrentTimestamp() {
    return static_cast<uint64_t>(std::time(nullptr));
}

std::string StreamConfigManager::escapeJSON(const std::string& str) {
    std::ostringstream oss;
    for (char c : str) {
        switch (c) {
            case '"':  oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:   oss << c; break;
        }
    }
    return oss.str();
}

// ============================================================================
// JSON Field Extraction Helpers
// ============================================================================

std::optional<std::string> StreamConfigManager::extractStringField(const std::string& json, const std::string& field) {
    std::string pattern = "\"" + field + "\"\\s*:\\s*\"([^\"]*)\"";
    std::regex re(pattern);
    std::smatch match;

    if (std::regex_search(json, match, re)) {
        return match[1].str();
    }

    return std::nullopt;
}

std::optional<uint64_t> StreamConfigManager::extractUInt64Field(const std::string& json, const std::string& field) {
    std::string pattern = "\"" + field + "\"\\s*:\\s*(\\d+)";
    std::regex re(pattern);
    std::smatch match;

    if (std::regex_search(json, match, re)) {
        return std::stoull(match[1].str());
    }

    return std::nullopt;
}

std::optional<uint32_t> StreamConfigManager::extractUInt32Field(const std::string& json, const std::string& field) {
    auto val = extractUInt64Field(json, field);
    if (val) {
        return static_cast<uint32_t>(*val);
    }
    return std::nullopt;
}

std::optional<uint16_t> StreamConfigManager::extractUInt16Field(const std::string& json, const std::string& field) {
    auto val = extractUInt64Field(json, field);
    if (val) {
        return static_cast<uint16_t>(*val);
    }
    return std::nullopt;
}

std::optional<uint8_t> StreamConfigManager::extractUInt8Field(const std::string& json, const std::string& field) {
    auto val = extractUInt64Field(json, field);
    if (val) {
        return static_cast<uint8_t>(*val);
    }
    return std::nullopt;
}

std::optional<double> StreamConfigManager::extractDoubleField(const std::string& json, const std::string& field) {
    std::string pattern = "\"" + field + "\"\\s*:\\s*([0-9.]+)";
    std::regex re(pattern);
    std::smatch match;

    if (std::regex_search(json, match, re)) {
        return std::stod(match[1].str());
    }

    return std::nullopt;
}

std::optional<bool> StreamConfigManager::extractBoolField(const std::string& json, const std::string& field) {
    std::string pattern = "\"" + field + "\"\\s*:\\s*(true|false)";
    std::regex re(pattern);
    std::smatch match;

    if (std::regex_search(json, match, re)) {
        return match[1].str() == "true";
    }

    return std::nullopt;
}

std::optional<int> StreamConfigManager::extractIntField(const std::string& json, const std::string& field) {
    std::string pattern = "\"" + field + "\"\\s*:\\s*(-?\\d+)";
    std::regex re(pattern);
    std::smatch match;

    if (std::regex_search(json, match, re)) {
        return std::stoi(match[1].str());
    }

    return std::nullopt;
}

} // namespace AES67
