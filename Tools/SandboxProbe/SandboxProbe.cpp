//
// SandboxProbe.cpp
// AES67 macOS Driver - Spike S1 (see Docs/Step2-Clocking-Plan.md)
//
// A throwaway AudioServerPlugIn that publishes no device. When Core Audio
// loads it into the driver host, it checks whether PTP can run there:
// binding UDP 319/320, joining the PTP multicast group, kernel monotonic
// receive timestamps, and sending. Results go to the unified log:
//
//   log show --last 5m --predicate 'subsystem == "com.aes67driver.probe"'
//
// Anything it sends uses TTL 0 (never leaves this Mac) and is not valid PTP
// (versionPTP 0), so no PTP device could act on it.
//

#include <aspl/Driver.hpp>
#include <aspl/Plugin.hpp>
#include <CoreAudio/AudioServerPlugIn.h>
#include <arpa/inet.h>
#include <mach/mach_time.h>
#include <netinet/in.h>
#include <os/log.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace {

constexpr const char* kPTPPrimaryGroup = "224.0.1.129";
constexpr uint16_t kEventPort = 319;
constexpr uint16_t kGeneralPort = 320;
constexpr int kListenSeconds = 30;

os_log_t probeLog() {
    static os_log_t log = os_log_create("com.aes67driver.probe", "S1");
    return log;
}

#define PROBE_LOG(...) os_log(probeLog(), __VA_ARGS__)

void logResult(const char* step, bool ok, int err) {
    if (ok) {
        PROBE_LOG("%{public}s: PASS", step);
    } else {
        PROBE_LOG("%{public}s: FAIL errno=%d (%{public}s)", step, err, strerror(err));
    }
}

double ticksToMicroseconds(int64_t ticks) {
    static mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        return tb;
    }();
    return static_cast<double>(ticks) * timebase.numer / timebase.denom / 1000.0;
}

// Bind a UDP socket the way a PTP slave would; returns fd or -1.
int openPort(uint16_t port, const char* label) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        logResult(label, false, errno);
        return -1;
    }
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    const bool bound = bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    logResult(label, bound, errno);
    if (!bound) {
        close(fd);
        return -1;
    }
    return fd;
}

void probeIdentity() {
    PROBE_LOG("identity: pid=%d uid=%d euid=%d gid=%d process=%{public}s",
              getpid(), getuid(), geteuid(), getgid(), getprogname());
}

void probeSend(int fd) {
    // versionPTP 0 and a reserved message type: no PTP stack will accept it
    const uint8_t marker[44] = {0x0F, 0x00};
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(kEventPort);
    dest.sin_addr.s_addr = inet_addr(kPTPPrimaryGroup);
    const unsigned char ttl = 0;
    const unsigned char loop = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    const bool sent = sendto(fd, marker, sizeof(marker), 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) > 0;
    logResult("send to 224.0.1.129:319 (TTL 0)", sent, errno);
}

void listen(int eventFd, int generalFd) {
    PROBE_LOG("listening %d s on 319/320", kListenSeconds);
    size_t received[2] = {0, 0};
    size_t withMonotonic = 0;
    double maxAgeUs = 0.0;
    double minAgeUs = 1e12;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kListenSeconds);
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(eventFd, &fds);
        if (generalFd >= 0) FD_SET(generalFd, &fds);
        timeval tv{0, 200000};
        if (select(std::max(eventFd, generalFd) + 1, &fds, nullptr, nullptr, &tv) <= 0) {
            continue;
        }

        for (int which = 0; which < 2; ++which) {
            const int fd = which == 0 ? eventFd : generalFd;
            if (fd < 0 || !FD_ISSET(fd, &fds)) continue;

            uint8_t buf[256];
            char control[256];
            iovec iov{buf, sizeof(buf)};
            msghdr msg{};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = control;
            msg.msg_controllen = sizeof(control);
            const ssize_t n = recvmsg(fd, &msg, 0);
            const uint64_t now = mach_absolute_time();
            if (n <= 0) continue;
            ++received[which];

            uint64_t kernelTicks = 0;
            for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP_MONOTONIC) {
                    std::memcpy(&kernelTicks, CMSG_DATA(c), sizeof(kernelTicks));
                }
            }
            if (kernelTicks != 0) {
                ++withMonotonic;
                const double ageUs = ticksToMicroseconds(static_cast<int64_t>(now - kernelTicks));
                maxAgeUs = std::max(maxAgeUs, ageUs);
                minAgeUs = std::min(minAgeUs, ageUs);
            }
            if (received[which] <= 3) {
                PROBE_LOG("rx port %d: %zd bytes, byte0=0x%02x byte1=0x%02x, monotonic ts %{public}s",
                          which == 0 ? kEventPort : kGeneralPort, n, buf[0], n > 1 ? buf[1] : 0,
                          kernelTicks ? "present" : "absent");
            }
        }
    }

    PROBE_LOG("summary: rx319=%zu rx320=%zu monotonicTimestamps=%zu kernelToUser=[%.1f..%.1f] us",
              received[0], received[1], withMonotonic,
              withMonotonic ? minAgeUs : 0.0, withMonotonic ? maxAgeUs : 0.0);
}

void runProbe() {
    PROBE_LOG("=== spike S1 probe start ===");
    probeIdentity();

    const int control = openPort(0, "bind ephemeral port (control)");
    if (control >= 0) close(control);

    const int eventFd = openPort(kEventPort, "bind UDP 319 (PTP event)");
    const int generalFd = openPort(kGeneralPort, "bind UDP 320 (PTP general)");
    if (eventFd < 0) {
        PROBE_LOG("=== spike S1 probe end (cannot bind 319) ===");
        if (generalFd >= 0) close(generalFd);
        return;
    }

    const int on = 1;
    errno = 0;
    logResult("setsockopt SO_TIMESTAMP_MONOTONIC",
              setsockopt(eventFd, SOL_SOCKET, SO_TIMESTAMP_MONOTONIC, &on, sizeof(on)) == 0, errno);
    if (generalFd >= 0) {
        setsockopt(generalFd, SOL_SOCKET, SO_TIMESTAMP_MONOTONIC, &on, sizeof(on));
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(kPTPPrimaryGroup);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    errno = 0;
    logResult("join 224.0.1.129 on port 319 socket",
              setsockopt(eventFd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == 0, errno);
    if (generalFd >= 0) {
        errno = 0;
        logResult("join 224.0.1.129 on port 320 socket",
                  setsockopt(generalFd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == 0, errno);
    }

    probeSend(eventFd);
    listen(eventFd, generalFd);

    close(eventFd);
    if (generalFd >= 0) close(generalFd);
    PROBE_LOG("=== spike S1 probe end ===");
}

} // namespace

extern "C" void* AES67SandboxProbeCreate(CFAllocatorRef, CFUUIDRef typeUUID) {
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) {
        return nullptr;
    }

    static std::shared_ptr<aspl::Driver> driver = [] {
        auto context = std::make_shared<aspl::Context>();
        auto plugin = std::make_shared<aspl::Plugin>(context);
        return std::make_shared<aspl::Driver>(context, plugin);
    }();

    // Probe off the HAL's thread so plug-in loading is never delayed
    static std::once_flag started;
    std::call_once(started, [] { std::thread(runProbe).detach(); });

    return driver->GetReference();
}
