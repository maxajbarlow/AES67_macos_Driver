/// @file PtpSocket.h
/// @brief A UDP socket for PTP messages, with kernel receive timestamps.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace AES67 {
namespace Ptp {

/// One PTP port's socket: the event port (319: Sync, Delay_Req) or the
/// general port (320: Follow_Up, Delay_Resp, Announce). Receive times come
/// from the kernel in host ticks (SO_TIMESTAMP_MONOTONIC): threads in the
/// Core Audio driver host can run tens of milliseconds late, so a userspace
/// time would be useless (Docs/Spikes/S1-PTP-Sandbox.md).
class Socket {
public:
    struct Options {
        uint16_t port{0};             // 0: send only (unbound)
        std::string bindAddress;      // "" = any address
        std::string group;            // multicast group to join, "" for none
        unsigned interfaceIndex{0};   // 0 lets the routing table choose
        uint8_t ttl{1};
    };

    Socket() = default;
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool open(const Options& options);
    void close();
    bool isOpen() const { return fd_ >= 0; }
    int fd() const { return fd_; }

    /// Non-blocking receive. @return bytes received, 0 if none waiting, -1 on error.
    /// @param hostTicks Kernel arrival time (mach ticks), 0 if unavailable.
    /// @param sourceAddress Sender's IPv4 address in network byte order.
    long receive(uint8_t* buffer, size_t size, uint64_t& hostTicks, uint32_t& sourceAddress);

    /// @param address IPv4 address in network byte order.
    bool sendTo(uint32_t address, uint16_t port, const uint8_t* data, size_t size);
    bool sendTo(const std::string& address, uint16_t port, const uint8_t* data, size_t size);

private:
    int fd_{-1};
};

} // namespace Ptp
} // namespace AES67
