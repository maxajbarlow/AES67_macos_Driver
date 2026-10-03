#ifndef SAP_LISTENER_H
#define SAP_LISTENER_H

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>

namespace AES67 {

struct SAPAnnouncement {
    std::string sessionDescription;  // The SDP content
    std::string sourceAddress;       // IP address of the announcer
    std::string sessionName;         // Name of the session
    std::string multicastAddress;    // Multicast address for the stream
    int port;                        // Port for the stream
    int ptpDomain;                   // PTP domain
    
    SAPAnnouncement() : port(0), ptpDomain(0) {}
};

using SAPAnnouncementCallback = std::function<void(const SAPAnnouncement&)>;

class SAPListener {
public:
    SAPListener();
    ~SAPListener();
    
    // Initialize the SAP listener
    bool initialize();
    
    // Start listening for SAP announcements
    bool start();
    
    // Stop listening
    void stop();
    
    // Register a callback to receive SAP announcements
    void registerAnnouncementCallback(const SAPAnnouncementCallback& callback);
    
    // Get the list of recently discovered streams
    std::vector<SAPAnnouncement> getDiscoveredStreams() const;

    // Parse one SAP packet (RFC 2974). Returns an announcement with an empty
    // sessionDescription if the packet is not an SDP announcement we support.
    static SAPAnnouncement parseAnnouncement(const char* data, size_t length, const std::string& sourceAddress);
    
private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace AES67

#endif // SAP_LISTENER_H