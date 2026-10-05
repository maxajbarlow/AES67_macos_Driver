#include "NetworkInterfaceDetection.h"
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <cstdio>

namespace AES67 {

namespace {

// inet_ntoa writes one static buffer, so monitors resolving interfaces on
// several threads could read each other's addresses
std::string ipv4String(const struct sockaddr* address) {
    char text[INET_ADDRSTRLEN] = {};
    const auto* ipv4 = reinterpret_cast<const struct sockaddr_in*>(address);
    return inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text)) ? std::string(text) : std::string();
}

} // namespace

std::string NetworkInterfaceDetection::getPrimaryEthernetInterface() {
    struct ifaddrs *ifaddrs_ptr, *ifa;
    std::string primaryInterface;
    
    if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            
            // Check if it's an active ethernet interface
            if ((ifa->ifa_flags & IFF_UP) && (ifa->ifa_flags & IFF_RUNNING) && 
                (ifa->ifa_flags & IFF_LOOPBACK) == 0) {
                
                // Check if it's an ethernet interface (AF_LINK indicates data link layer info)
                if (ifa->ifa_addr->sa_family == AF_INET) {  // IPv4 interfaces
                    std::string name(ifa->ifa_name);
                    
                    // Look for ethernet interfaces (typically named en0, en1, etc.)
                    // But also check for other common names
                    if (name.substr(0, 2) == "en" ||  // Ethernet
                        name.substr(0, 4) == "eth" || // Alternative naming
                        name.substr(0, 4) == "thun" || // Thunderbolt Ethernet
                        name.substr(0, 3) == "usb") { // USB Ethernet
                    
                        // Prefer interfaces with actual IP addresses (not link-local)
                        std::string ip = ipv4String(ifa->ifa_addr);
                        
                        // Skip link-local addresses (169.254.x.x)
                        if (ip.substr(0, 7) != "169.254") {
                            primaryInterface = name;
                            break;  // Return first active ethernet interface with valid IP
                        }
                    }
                }
            }
        }
        freeifaddrs(ifaddrs_ptr);
    }
    
    return primaryInterface;
}

std::vector<std::string> NetworkInterfaceDetection::getAllInterfaces() {
    std::vector<std::string> interfaces;
    struct ifaddrs *ifaddrs_ptr, *ifa;
    
    if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            if (ifa->ifa_addr->sa_family == AF_INET) {  // IPv4 interfaces
                std::string name(ifa->ifa_name);
                
                // Check if we already have this interface
                bool found = false;
                for (const auto& existing : interfaces) {
                    if (existing == name) {
                        found = true;
                        break;
                    }
                }
                
                if (!found) {
                    interfaces.push_back(name);
                }
            }
        }
        freeifaddrs(ifaddrs_ptr);
    }
    
    return interfaces;
}

bool NetworkInterfaceDetection::isInterfaceActive(const std::string& interfaceName) {
    struct ifaddrs *ifaddrs_ptr, *ifa;
    bool isActive = false;
    
    if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_name != nullptr && interfaceName == ifa->ifa_name) {
                if ((ifa->ifa_flags & IFF_UP) && (ifa->ifa_flags & IFF_RUNNING)) {
                    isActive = true;
                    break;
                }
            }
        }
        freeifaddrs(ifaddrs_ptr);
    }
    
    return isActive;
}

bool NetworkInterfaceDetection::isEthernetInterface(const std::string& interfaceName) {
    struct ifaddrs *ifaddrs_ptr, *ifa;
    bool isEthernet = false;
    
    if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_name != nullptr && interfaceName == ifa->ifa_name) {
                // Check if it has a link layer address (MAC address)
                if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_LINK) {
                    // AF_LINK indicates it's a data link layer interface (like Ethernet)
                    isEthernet = true;
                    break;
                }
            }
        }
        freeifaddrs(ifaddrs_ptr);
    }
    
    return isEthernet;
}

std::string NetworkInterfaceDetection::getInterfaceIPAddress(const std::string& interfaceName) {
    struct ifaddrs *ifaddrs_ptr, *ifa;
    std::string ipAddress;

    if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_name != nullptr && interfaceName == ifa->ifa_name) {
                if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
                    ipAddress = ipv4String(ifa->ifa_addr);
                    break;
                }
            }
        }
        freeifaddrs(ifaddrs_ptr);
    }

    return ipAddress;
}

std::string NetworkInterfaceDetection::getInterfaceForIPAddress(const std::string& ipAddress) {
    struct ifaddrs *ifaddrs_ptr, *ifa;
    std::string name;

    if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_name && ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET &&
                ipAddress == ipv4String(ifa->ifa_addr)) {
                name = ifa->ifa_name;
                break;
            }
        }
        freeifaddrs(ifaddrs_ptr);
    }

    return name;
}

std::string NetworkInterfaceDetection::getInterfaceMACAddress(const std::string& interfaceName) {
    struct ifaddrs *ifaddrs_ptr, *ifa;
    std::string mac;

    if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || interfaceName != ifa->ifa_name || !ifa->ifa_addr ||
                ifa->ifa_addr->sa_family != AF_LINK) {
                continue;
            }
            const auto* link = reinterpret_cast<const struct sockaddr_dl*>(ifa->ifa_addr);
            if (link->sdl_alen != 6) {
                break;  // no hardware address (e.g. loopback)
            }
            const auto* bytes = reinterpret_cast<const unsigned char*>(LLADDR(link));
            char text[18];
            snprintf(text, sizeof(text), "%02X-%02X-%02X-%02X-%02X-%02X", bytes[0], bytes[1], bytes[2], bytes[3],
                     bytes[4], bytes[5]);
            mac = text;
            break;
        }
        freeifaddrs(ifaddrs_ptr);
    }

    return mac;
}

std::optional<NetworkInterfaceDetection::InterfaceState> NetworkInterfaceDetection::currentState(
    const std::string& setting) {
    std::string name;
    struct in_addr probe {};
    if (setting.empty() || setting == "auto") {
        name = getPrimaryEthernetInterface();
    } else if (inet_pton(AF_INET, setting.c_str(), &probe) == 1) {
        name = getInterfaceForIPAddress(setting);
    } else {
        name = setting;
    }
    if (name.empty()) {
        return std::nullopt;
    }

    InterfaceState state;
    state.name = name;
    state.index = if_nametoindex(name.c_str());
    if (state.index == 0) {
        return std::nullopt;
    }

    struct ifaddrs *ifaddrs_ptr, *ifa;
    if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || name != ifa->ifa_name) {
                continue;
            }
            state.running = state.running || ((ifa->ifa_flags & IFF_UP) && (ifa->ifa_flags & IFF_RUNNING));
            if (state.ipv4.empty() && ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
                state.ipv4 = ipv4String(ifa->ifa_addr);
            }
        }
        freeifaddrs(ifaddrs_ptr);
    }
    return state;
}

bool NetworkInterfaceDetection::socketInterfaceIndex(const std::string& setting, unsigned& index,
                                                     std::string& description) {
    if (setting.empty() || setting == "auto") {
        index = 0;
        description = "default route";
        return true;
    }
    const auto state = currentState(setting);
    if (!state) {
        index = 0;
        description = setting + " (not present)";
        return false;
    }
    index = state->index;
    description = state->name + (state->ipv4.empty() ? "" : " " + state->ipv4);
    return true;
}

std::string NetworkInterfaceDetection::resolveIPv4Address(const std::string& interfaceSetting) {
    const auto state = currentState(interfaceSetting);
    return state ? state.value().ipv4 : std::string{};
}

bool NetworkInterfaceDetection::supportsMulticast(const std::string& interfaceName) {
    struct ifaddrs *ifaddrs_ptr, *ifa;
    bool hasMulticast = false;

    if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_name != nullptr && interfaceName == ifa->ifa_name) {
                // Check if the interface supports multicast
                if (ifa->ifa_flags & IFF_MULTICAST) {
                    hasMulticast = true;
                    break;
                }
            }
        }
        freeifaddrs(ifaddrs_ptr);
    }

    return hasMulticast;
}

std::vector<std::string> NetworkInterfaceDetection::getMulticastCapableInterfaces() {
    std::vector<std::string> interfaces;
    struct ifaddrs *ifaddrs_ptr, *ifa;

    if (getifaddrs(&ifaddrs_ptr) == 0) {
        for (ifa = ifaddrs_ptr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;

            // Must be IPv4, active, and support multicast
            if (ifa->ifa_addr->sa_family == AF_INET &&
                (ifa->ifa_flags & IFF_UP) &&
                (ifa->ifa_flags & IFF_RUNNING) &&
                (ifa->ifa_flags & IFF_MULTICAST) &&
                (ifa->ifa_flags & IFF_LOOPBACK) == 0) {

                std::string name(ifa->ifa_name);

                // Skip link-local addresses (169.254.x.x)
                std::string ip = ipv4String(ifa->ifa_addr);
                if (ip.substr(0, 7) == "169.254") {
                    continue;
                }

                // Check if we already have this interface
                bool found = false;
                for (const auto& existing : interfaces) {
                    if (existing == name) {
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    interfaces.push_back(name);
                }
            }
        }
        freeifaddrs(ifaddrs_ptr);
    }

    return interfaces;
}

std::string NetworkInterfaceDetection::detectPTPInterface() {
    auto interfaces = getMulticastCapableInterfaces();

    if (interfaces.empty()) {
        // No suitable interfaces found, return default fallback
        return "en0";
    }

    // First pass: Look for dedicated AES67/audio interface naming conventions
    for (const auto& iface : interfaces) {
        std::string lowerName = iface;
        // Convert to lowercase for case-insensitive comparison
        for (auto& c : lowerName) {
            c = ::tolower(c);
        }

        if (lowerName.find("aes67") != std::string::npos ||
            lowerName.find("audio") != std::string::npos ||
            lowerName.find("ravenna") != std::string::npos ||
            lowerName.find("dante") != std::string::npos) {
            return iface;
        }
    }

    // Second pass: Prefer standard Ethernet interfaces (en0, en1, etc.)
    for (const auto& iface : interfaces) {
        if (iface.substr(0, 2) == "en") {
            return iface;
        }
    }

    // Third pass: Accept other Ethernet-like interfaces
    for (const auto& iface : interfaces) {
        if (iface.substr(0, 3) == "eth" ||      // Alternative ethernet naming
            iface.substr(0, 4) == "thun" ||     // Thunderbolt ethernet
            iface.substr(0, 3) == "usb") {      // USB ethernet
            return iface;
        }
    }

    // Fall back to first available interface
    return interfaces[0];
}

} // namespace AES67