#ifndef NETWORK_INTERFACE_DETECTION_H
#define NETWORK_INTERFACE_DETECTION_H

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