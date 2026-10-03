//
// TestSAPAnnouncer.cpp
// AES67 macOS Driver
// SAP announcements (RFC 2974) for TX streams. Live tests use a test port and
// TTL 0, so nothing reaches the network or a SAP listener on port 9875.
//

#include "../NetworkEngine/Discovery/SAPAnnouncer.h"
#include "../NetworkEngine/Discovery/SAPListener.h"
#include "../NetworkEngine/NetworkInterfaceDetection.h"
#include "SapTestSupport.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace AES67;
using namespace AES67::TestSupport;

namespace {

int checksPassed = 0;
int checksFailed = 0;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (condition) {                                                       \
            ++checksPassed;                                                    \
        } else {                                                               \
            ++checksFailed;                                                    \
            std::cerr << "  FAIL: " << message << " (" << __FILE__ << ":"      \
                      << __LINE__ << ")" << std::endl;                         \
        }                                                                      \
    } while (0)

constexpr uint16_t kTestPort = kTestSapPort;

const std::string kSdp =
    "v=0\n"
    "o=- 1234 1 IN IP4 10.0.0.5\n"
    "s=Announced\n"
    "c=IN IP4 239.69.99.50/0\n"
    "t=0 0\n"
    "m=audio 5004 RTP/AVP 97\n"
    "a=rtpmap:97 L24/48000/2\n"
    "a=recvonly\n";

uint16_t hashOf(const std::vector<uint8_t>& packet) {
    return static_cast<uint16_t>((packet[2] << 8) | packet[3]);
}

void testPacketFormat() {
    std::cout << "SAP announcement packet format" << std::endl;
    const uint32_t origin = inet_addr("10.0.0.5");
    const auto packet = SAPAnnouncer::buildPacket(kSdp, origin, false);

    CHECK(packet.size() > 8 && (packet[0] >> 5) == 1, "version 1");
    CHECK((packet[0] & 0x04) == 0, "the T bit is clear for an announcement");
    CHECK((packet[0] & 0x10) == 0, "the A bit is clear for an IPv4 origin");
    CHECK(packet[1] == 0, "no authentication data");
    CHECK(hashOf(packet) != 0, "the message ID hash is never 0");
    uint32_t carriedOrigin = 0;
    std::memcpy(&carriedOrigin, packet.data() + 4, 4);
    CHECK(carriedOrigin == origin, "the originating source is the announcing address");

    const SAPAnnouncement parsed = SAPListener::parseAnnouncement(
        reinterpret_cast<const char*>(packet.data()), packet.size(), "10.0.0.5");
    CHECK(parsed.sessionDescription == kSdp, "our own listener reads back the exact SDP");
    CHECK(parsed.sessionName == "Announced" && parsed.port == 5004, "and its session fields");
}

void testDeletionPacket() {
    std::cout << "SAP deletion packet" << std::endl;
    const uint32_t origin = inet_addr("10.0.0.5");
    const auto announce = SAPAnnouncer::buildPacket(kSdp, origin, false);
    const auto deletion = SAPAnnouncer::buildPacket(kSdp, origin, true);
    CHECK((deletion[0] & 0x04) != 0, "the T bit marks a deletion");
    CHECK(hashOf(deletion) == hashOf(announce), "a deletion carries the announcement's message ID hash");
    CHECK(SAPListener::parseAnnouncement(reinterpret_cast<const char*>(deletion.data()), deletion.size(), "")
              .sessionDescription.empty(),
          "a listener does not mistake a deletion for an announcement");
}

void testHashFollowsContent() {
    std::cout << "SAP message ID hash follows the SDP" << std::endl;
    std::string changed = kSdp;
    changed.replace(changed.find("s=Announced"), 11, "s=Changed!!");
    CHECK(SAPAnnouncer::messageIdHash(kSdp) == SAPAnnouncer::messageIdHash(kSdp), "the same SDP hashes the same");
    CHECK(SAPAnnouncer::messageIdHash(kSdp) != SAPAnnouncer::messageIdHash(changed),
          "a changed SDP gets a new hash (RFC 2974)");
}

std::string localAddress() {
    return NetworkInterfaceDetection::getInterfaceIPAddress(NetworkInterfaceDetection::getPrimaryEthernetInterface());
}

void testAnnounceRepeatAndWithdraw() {
    std::cout << "SAP announces at once, repeats with jitter, and withdraws" << std::endl;
    const std::string address = localAddress();
    if (address.empty()) {
        std::cout << "  skipped: no network interface" << std::endl;
        return;
    }
    SAPAnnouncer::Config config;
    config.port = kTestPort;
    config.interval = std::chrono::milliseconds(300);
    SapCapture capture(config.group, config.port, address);
    SAPAnnouncer announcer(config);

    const auto start = std::chrono::steady_clock::now();
    const StreamID id = StreamID::generate();
    announcer.announce(id, kSdp, address, 0);
    CHECK(announcer.sessionCount() == 1, "one session announced");
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    announcer.withdraw(id);
    const auto withdrawnAt = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    const auto packets = capture.packets();

    size_t announcements = 0;
    size_t deletions = 0;
    size_t afterWithdrawal = 0;
    bool sameHash = true;
    for (const auto& p : packets) {
        sameHash = sameHash && hashOf(p.bytes) == hashOf(packets.front().bytes);
        if (isDeletion(p)) {
            ++deletions;
        } else {
            ++announcements;
            if (p.at > withdrawnAt) ++afterWithdrawal;
        }
    }
    CHECK(!packets.empty() && packets.front().at - start < std::chrono::milliseconds(100),
          "the first announcement goes out at once");
    // Nominal 300 ms, jittered to 200-400 ms: about 7 in 2.1 s (5-11 allowed)
    CHECK(announcements >= 5 && announcements <= 11, "re-announced every interval, with jitter (got " << announcements << ")");
    CHECK(deletions == 1, "withdrawing sends one deletion (got " << deletions << ")");
    CHECK(afterWithdrawal == 0, "nothing is announced after withdrawal");
    CHECK(sameHash, "announcements and the deletion share the session's message ID hash");
    CHECK(announcer.sessionCount() == 0, "no sessions left");

    // Intervals must vary (RFC 2974 randomises them)
    std::vector<long long> gaps;
    std::chrono::steady_clock::time_point previous{};
    for (const auto& p : packets) {
        if (isDeletion(p)) continue;
        if (previous.time_since_epoch().count() != 0) {
            gaps.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(p.at - previous).count());
        }
        previous = p.at;
    }
    bool varied = false;
    for (size_t i = 1; i < gaps.size(); ++i) varied = varied || std::llabs(gaps[i] - gaps[0]) > 15;
    CHECK(varied, "re-announcement intervals should be jittered");
}

void testDestructionWithdrawsEverything() {
    std::cout << "Destroying the announcer withdraws every session" << std::endl;
    const std::string address = localAddress();
    if (address.empty()) {
        std::cout << "  skipped: no network interface" << std::endl;
        return;
    }
    SAPAnnouncer::Config config;
    config.port = kTestPort;
    SapCapture capture(config.group, config.port, address);
    {
        SAPAnnouncer announcer(config);
        announcer.announce(StreamID::generate(), kSdp, address, 0);
        std::string second = kSdp;
        second.replace(second.find("s=Announced"), 11, "s=Second!!!");
        announcer.announce(StreamID::generate(), second, address, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    size_t deletions = 0;
    for (const auto& p : capture.packets()) deletions += isDeletion(p) ? 1 : 0;
    CHECK(deletions == 2, "both sessions should be deleted (got " << deletions << ")");
}

} // namespace

int main() {
    testPacketFormat();
    testDeletionPacket();
    testHashFollowsContent();
    testAnnounceRepeatAndWithdraw();
    testDestructionWithdrawsEverything();

    std::cout << "\nSAP announcer: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
