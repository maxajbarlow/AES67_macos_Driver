#ifndef NETWORK_INTERFACE_DETECTION_H
#define NETWORK_INTERFACE_DETECTION_H

#include <optional>
#include <string>
#include <vector>

namespace AES67 {

/**
 * Utility class for detecting network interfaces
 * 
 * Critical for proper AES67 operation since hardcoded interface names don't work reliably
 */
class NetworkInterfaceDetection {
public:
    /// An interface as a stream uses it. Comparing two snapshots tells
    /// whether a stream's multicast memberships and sends need renewing.
    struct InterfaceState {
        std::string name;    // "en0"
        unsigned index{0};   // if_nametoindex: what sockets join and send by
        std::string ipv4;    // current address (empty if none yet)
        bool running{false}; // IFF_UP and IFF_RUNNING (link up)

        bool operator==(const InterfaceState& other) const {
            return name == other.name && index == other.index && ipv4 == other.ipv4 && running == other.running;
        }
        bool operator!=(const InterfaceState& other) const { return !(*this == other); }
    };

    /**
     * Current state of the interface a stream's setting names: an interface
     * name ("en0"), an IPv4 address (the interface holding it), or "" / "auto"
     * (the primary ethernet interface). Resolved afresh on every call, so a
     * new address, link state or interface index is always seen.
     * @return The interface's state, or nullopt if no interface matches
     */
    static std::optional<InterfaceState> currentState(const std::string& setting);

    /**
     * Interface index a socket should join or send on for a stream's setting:
     * 0 (the kernel's choice) for "" / "auto", otherwise the named interface's
     * current index. Resolved when a stream starts, never cached.
     * @param description Set to the interface used, for logs
     * @return false if the setting names an interface that does not exist now
     */
    static bool socketInterfaceIndex(const std::string& setting, unsigned& index, std::string& description);

    /**
     * Get the primary ethernet interface
     * @return Name of the primary ethernet interface (e.g., "en0", "en1"), or empty string if not found
     */
    static std::string getPrimaryEthernetInterface();
    
    /**
     * Get all available network interfaces
     * @return Vector of interface names
     */
    static std::vector<std::string> getAllInterfaces();
    
    /**
     * Check if an interface is active (up and running)
     * @param interfaceName Name of the interface to check
     * @return true if interface is active, false otherwise
     */
    static bool isInterfaceActive(const std::string& interfaceName);
    
    /**
     * Check if an interface is an ethernet interface
     * @param interfaceName Name of the interface to check
     * @return true if interface is ethernet, false otherwise
     */
    static bool isEthernetInterface(const std::string& interfaceName);
    
    /**
     * Get the IP address of an interface
     * @param interfaceName Name of the interface
     * @return IP address as string, or empty string if not found
     */
    static std::string getInterfaceIPAddress(const std::string& interfaceName);

    /**
     * Name of the interface holding an IPv4 address
     * @return Interface name, or empty string if no interface has it
     */
    static std::string getInterfaceForIPAddress(const std::string& ipAddress);

    /**
     * Hardware (MAC) address of an interface, dash-separated upper-case hex
     * as SDP's ts-refclk:localmac= expects (e.g. "00-1D-C1-D1-7B-F3")
     * @return MAC address, or empty string if the interface has none
     */
    static std::string getInterfaceMACAddress(const std::string& interfaceName);

    /**
     * IPv4 address for an interface setting as streams store it: an
     * interface name ("en0"), an IPv4 address, or empty for the primary
     * ethernet interface
     * @return IPv4 address, or empty string if it cannot be resolved
     */
    static std::string resolveIPv4Address(const std::string& interfaceSetting);

    /**
     * Check if an interface supports multicast
     * @param interfaceName Name of the interface to check
     * @return true if interface supports multicast, false otherwise
     */
    static bool supportsMulticast(const std::string& interfaceName);

    /**
     * Get all multicast-capable ethernet interfaces
     * @return Vector of interface names that support multicast and are active
     */
    static std::vector<std::string> getMulticastCapableInterfaces();

    /**
     * Detect the best interface for PTP/AES67 operation
     * Prefers interfaces with audio/aes67 naming, then ethernet interfaces
     * @return Name of the best interface for PTP, or empty string if none found
     */
    static std::string detectPTPInterface();
};

} // namespace AES67

#endif // NETWORK_INTERFACE_DETECTION_H