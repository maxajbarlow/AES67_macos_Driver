/// @file PtpSettings.h
/// @brief Whether and how the driver runs PTP (ptp.json).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AES67 {
namespace Ptp {

/// The driver's PTP settings. PTP is off unless a settings file turns it
/// on: a time receiver sends Delay_Req packets onto the network, so it
/// never starts unasked. Anything malformed leaves it off.
///
/// ptp.json, next to streams.json:
///   { "enabled": true, "interface": "en0", "domain": 0, "hybrid": false }
/// "interface" is a name, an address, or "" for the primary interface;
/// "hybrid" sends Delay_Req unicast to the master.
struct Settings {
    bool enabled{false};
    std::string networkInterface;
    uint8_t domain{0};
    bool unicastDelayRequests{false};

    /// Parse a settings file's contents. Off (defaults) if malformed.
    static Settings parse(const std::string& json, std::string* error = nullptr);

    /// Read the first settings file found: AES67_PTP_CONFIG_PATH if set (and
    /// then only that), otherwise the user's and then the system's
    /// Application Support/AES67Driver/ptp.json.
    /// Off if there is none, or if it is malformed (`error` says why).
    static Settings load(std::string* foundAt = nullptr, std::string* error = nullptr);

    static std::vector<std::string> searchPaths();
};

} // namespace Ptp
} // namespace AES67
