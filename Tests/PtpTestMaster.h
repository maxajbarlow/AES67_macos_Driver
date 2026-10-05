//
// PtpTestMaster.h
// AES67 macOS Driver
// A scripted PTP master on lo0 for tests: its own clock (an epoch and a
// skew against host time), Announce and Sync on a schedule, and answers to
// Delay_Req by multicast or unicast. Test ports and TTL 0, so nothing
// reaches the network.
//

#pragma once

#include "../NetworkEngine/PTP/PtpMessages.h"
#include "../NetworkEngine/PTP/PtpSocket.h"
#include "../NetworkEngine/PTP/PtpTimeReceiver.h"
#include "../NetworkEngine/Clock/HostTime.h"
#include <arpa/inet.h>
#include <poll.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <net/if.h>
#include <thread>
#include <vector>

namespace AES67 {
namespace PtpTest {

using namespace AES67::Ptp;

inline constexpr uint16_t kEventPort = 31319;
inline constexpr uint16_t kGeneralPort = 31320;
inline constexpr const char* kGroup = "224.0.1.129";

inline uint64_t hostNanos() { return HostTimebase::current().ticksToNanos(hostTimeNow()); }

// A PTP master on lo0. Its clock: epoch + (1 + skew) * host time.
class ScriptedMaster {
public:
    struct Settings {
        uint8_t identityByte{0x01};
        uint8_t priority1{128};
        uint64_t epochNs{261432ULL * 1000000000ULL};  // a console's power-on time
        double skewPpm{100.0};
        bool twoStep{true};
        int64_t followUpCorrectionNs{0};  // carried in Follow_Up (or Sync, one-step)
        int64_t delayRespCorrectionNs{0};
        int8_t logSyncInterval{-4};       // 16 per second
        int8_t logAnnounceInterval{-2};   // 4 per second
        int8_t logDelayReqInterval{-3};   // tells the receiver: 8 Delay_Req per second
        uint16_t eventPort{kEventPort};
        uint16_t generalPort{kGeneralPort};
    };

    explicit ScriptedMaster(Settings settings) : s_(settings) {
        identity_ = PortIdentity{ClockIdentity{{0x00, 0x1D, 0xC1, 0xFF, 0xFE, 0x00, 0x00, s_.identityByte}}, 1};
        const unsigned lo0 = if_nametoindex("lo0");
        send_.open(Socket::Options{0, "", "", lo0, 0});
        multicastEvent_.open(Socket::Options{s_.eventPort, "", kGroup, lo0, 0});
        unicastEvent_.open(Socket::Options{s_.eventPort, "127.0.0.1", "", lo0, 0});
        thread_ = std::thread([this] { run(); });
    }
    ~ScriptedMaster() { stop(); }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    uint64_t clockAt(uint64_t hostNs) const {
        return s_.epochNs + static_cast<uint64_t>(std::llround(static_cast<double>(hostNs) * (1.0 + s_.skewPpm * 1e-6)));
    }
    const PortIdentity& identity() const { return identity_; }
    uint64_t multicastDelayRequests() const { return multicastRequests_.load(); }
    uint64_t unicastDelayRequests() const { return unicastRequests_.load(); }

private:
    static Timestamp timestampOf(uint64_t ns) { return Timestamp{ns / 1000000000ULL, static_cast<uint32_t>(ns % 1000000000ULL)}; }

    void sendGroup(uint16_t port, const std::vector<uint8_t>& bytes) { send_.sendTo(kGroup, port, bytes.data(), bytes.size()); }

    void sendSync() {
        const uint16_t seq = syncSequence_++;
        const uint64_t t1 = clockAt(hostNanos());  // just before it leaves
        const int64_t c = s_.followUpCorrectionNs;
        // The correction field carries part of t1 (as a transparent clock
        // would): t1 = origin + correction
        const Timestamp origin = timestampOf(t1 - static_cast<uint64_t>(c));
        const int64_t scaled = c * 65536;
        if (s_.twoStep) {
            sendGroup(s_.eventPort, buildSync(identity_, 0, seq, s_.logSyncInterval, true, Timestamp{}));
            sendGroup(s_.generalPort, buildFollowUp(identity_, 0, seq, s_.logSyncInterval, origin, scaled));
        } else {
            sendGroup(s_.eventPort, buildSync(identity_, 0, seq, s_.logSyncInterval, false, origin, scaled));
        }
    }

    void sendAnnounce() {
        AnnounceBody body;
        body.grandmasterIdentity = identity_.clock;
        body.grandmasterPriority1 = s_.priority1;
        sendGroup(s_.generalPort, buildAnnounce(identity_, 0, announceSequence_++, s_.logAnnounceInterval, body));
    }

    void answerDelayRequests(Socket& socket, bool unicast) {
        uint8_t buffer[256];
        uint64_t ticks = 0;
        uint32_t source = 0;
        long n;
        while ((n = socket.receive(buffer, sizeof(buffer), ticks, source)) > 0) {
            const auto m = parse(buffer, static_cast<size_t>(n));
            if (!m || m->header.type != MessageType::DelayReq || m->header.source.clock == identity_.clock) continue;
            (unicast ? unicastRequests_ : multicastRequests_).fetch_add(1);
            const uint64_t t4 = clockAt(HostTimebase::current().ticksToNanos(ticks));
            // The correction field carries part of t4: t4 = receive - correction
            const int64_t c = s_.delayRespCorrectionNs;
            const auto resp = buildDelayResp(identity_, 0, m->header.sequenceId, s_.logDelayReqInterval,
                                             timestampOf(t4 + static_cast<uint64_t>(c)), m->header.source, c * 65536);
            if (unicast) {
                send_.sendTo(source, s_.generalPort, resp.data(), resp.size());
            } else {
                sendGroup(s_.generalPort, resp);
            }
        }
    }

    void run() {
        const uint64_t syncNs = static_cast<uint64_t>(std::ldexp(1e9, s_.logSyncInterval));
        const uint64_t announceNs = static_cast<uint64_t>(std::ldexp(1e9, s_.logAnnounceInterval));
        uint64_t nextSync = hostNanos();
        uint64_t nextAnnounce = nextSync;
        while (running_) {
            const uint64_t now = hostNanos();
            if (now >= nextAnnounce) { sendAnnounce(); nextAnnounce += announceNs; }
            if (now >= nextSync) { sendSync(); nextSync += syncNs; }
            pollfd fds[2] = {{multicastEvent_.fd(), POLLIN, 0}, {unicastEvent_.fd(), POLLIN, 0}};
            ::poll(fds, 2, 2);
            answerDelayRequests(multicastEvent_, false);
            answerDelayRequests(unicastEvent_, true);
        }
    }

    Settings s_;
    PortIdentity identity_;
    Socket send_, multicastEvent_, unicastEvent_;
    uint16_t syncSequence_{0};
    uint16_t announceSequence_{0};
    std::atomic<uint64_t> multicastRequests_{0};
    std::atomic<uint64_t> unicastRequests_{0};
    std::atomic<bool> running_{true};
    std::thread thread_;
};

/// A receiver on lo0 with the test ports.
inline TimeReceiver::Config receiverConfig(bool unicast = false) {
    TimeReceiver::Config c;
    c.networkInterface = "lo0";
    c.eventPort = kEventPort;
    c.generalPort = kGeneralPort;
    c.unicastDelayRequests = unicast;
    c.ttl = 0;
    c.identity = ClockIdentity{{0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0xDD, 0xEE, 0xFF}};
    return c;
}

} // namespace PtpTest
} // namespace AES67
