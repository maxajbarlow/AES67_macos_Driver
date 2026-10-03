//
// TestSAPListener.cpp
// AES67 macOS Driver
// SAP announcement parsing (RFC 2974): header, optional payload type and the
// SDP that follows it.
//

#include "../NetworkEngine/Discovery/SAPListener.h"
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace AES67;

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

const std::string kSdp =
    "v=0\r\n"
    "o=- 1 1 IN IP4 10.46.70.20\r\n"
    "s=WING\r\n"
    "c=IN IP4 239.69.218.192/32\r\n"
    "t=0 0\r\n"
    "m=audio 5004 RTP/AVP 103\r\n"
    "a=rtpmap:103 L24/48000/8\r\n";

// SAP header: V=1, A (IPv6 origin), T=0 (announce), no auth, then the
// originating source, an optional payload type and the SDP
std::vector<char> sapPacket(bool ipv6Origin, const std::string& payloadType, const std::string& sdp) {
    std::vector<char> packet;
    packet.push_back(static_cast<char>((1 << 5) | (ipv6Origin ? 0x10 : 0x00)));
    packet.push_back(0);                       // auth length
    packet.push_back(0x12);                    // message id hash
    packet.push_back(0x34);
    const size_t originBytes = ipv6Origin ? 16 : 4;
    for (size_t i = 0; i < originBytes; ++i) packet.push_back(static_cast<char>(10 + i));
    if (!payloadType.empty()) {
        packet.insert(packet.end(), payloadType.begin(), payloadType.end());
        packet.push_back('\0');
    }
    packet.insert(packet.end(), sdp.begin(), sdp.end());
    return packet;
}

SAPAnnouncement parse(const std::vector<char>& packet) {
    return SAPListener::parseAnnouncement(packet.data(), packet.size(), "10.46.70.20");
}

void testPayloadTypeIsNotPartOfTheSdp() {
    std::cout << "SAP payload type is stripped from the SDP" << std::endl;
    const SAPAnnouncement a = parse(sapPacket(false, "application/sdp", kSdp));
    CHECK(a.sessionDescription == kSdp, "the SDP should start at v=0, without the payload type (got "
                                            << a.sessionDescription.size() << " bytes)");
    CHECK(a.sessionName == "WING" && a.port == 5004, "session fields should parse");
}

void testPayloadTypeIsOptional() {
    std::cout << "SAP payload type is optional" << std::endl;
    const SAPAnnouncement a = parse(sapPacket(false, "", kSdp));
    CHECK(a.sessionDescription == kSdp, "an SDP straight after the header should be accepted");
}

void testIpv6Origin() {
    std::cout << "SAP with an IPv6 originating source" << std::endl;
    const SAPAnnouncement a = parse(sapPacket(true, "application/sdp", kSdp));
    CHECK(a.sessionDescription == kSdp, "a 16-byte origin should not be read as SDP");
}

void testOtherPayloadTypesIgnored() {
    std::cout << "SAP payloads other than SDP are ignored" << std::endl;
    const SAPAnnouncement a = parse(sapPacket(false, "application/x-not-sdp", kSdp));
    CHECK(a.sessionDescription.empty(), "a non-SDP payload type should be rejected");
}

} // namespace

int main() {
    testPayloadTypeIsNotPartOfTheSdp();
    testPayloadTypeIsOptional();
    testIpv6Origin();
    testOtherPayloadTypesIgnored();

    std::cout << "\nSAP listener: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
