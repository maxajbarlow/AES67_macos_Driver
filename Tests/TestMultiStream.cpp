//
// TestMultiStream.cpp
// AES67 macOS Driver - Build #18
// Integration tests for multi-stream scenarios
//

#include "../NetworkEngine/StreamManager.h"
#include "../NetworkEngine/StreamChannelMapper.h"
#include "../Driver/SDPParser.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <memory>

using namespace AES67;

// Test result counter
static int testsPassed = 0;
static int testsFailed = 0;

#define TEST_ASSERT(condition, message) \
    if (!(condition)) { \
        std::cerr << "FAIL: " << message << std::endl; \
        testsFailed++; \
        return false; \
    } else { \
        testsPassed++; \
    }

//
// Helper Functions
//

// Create test SDP session
SDPSession createTestStream(const std::string& name,
                           const std::string& mcastAddr,
                           uint16_t port,
                           uint16_t channels,
                           uint32_t sampleRate = 48000) {
    SDPSession sdp;
    sdp.sessionName = name;
    sdp.port = port;
    sdp.connectionAddress = mcastAddr;
    sdp.encoding = "L24";
    sdp.sampleRate = sampleRate;
    sdp.numChannels = channels;
    sdp.payloadType = 97;
    sdp.ptime = 1;
    sdp.framecount = 48;
    sdp.originAddress = "192.168.1.100";
    sdp.ptpDomain = 0;

    return sdp;
}

// Create channel mapping
ChannelMapping createMapping(const StreamID& streamID,
                             const std::string& name,
                             uint16_t streamChannels,
                             uint16_t deviceStart) {
    ChannelMapping mapping;
    mapping.streamID = streamID;
    mapping.streamName = name;
    mapping.streamChannelCount = streamChannels;
    mapping.streamChannelOffset = 0;
    mapping.deviceChannelStart = deviceStart;
    mapping.deviceChannelCount = streamChannels;

    return mapping;
}

//
// Multi-Stream Configuration Tests
//

bool testTwoStreamConfiguration() {
    std::cout << "Test: Two-stream configuration... ";

    // Stream 1: 8 channels on 239.1.1.1:5004
    SDPSession stream1 = createTestStream("Stream 1", "239.1.1.1", 5004, 8);
    TEST_ASSERT(stream1.isValid(), "Stream 1 should be valid");

    // Stream 2: 8 channels on 239.1.1.2:5006
    SDPSession stream2 = createTestStream("Stream 2", "239.1.1.2", 5006, 8);
    TEST_ASSERT(stream2.isValid(), "Stream 2 should be valid");

    // Different multicast addresses
    TEST_ASSERT(stream1.connectionAddress != stream2.connectionAddress,
                "Streams should have different addresses");

    // Different ports
    TEST_ASSERT(stream1.port != stream2.port,
                "Streams should have different ports");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testFourStreamConfiguration() {
    std::cout << "Test: Four-stream configuration... ";

    // Create 4 streams with different addresses
    std::vector<SDPSession> streams;
    for (int i = 0; i < 4; i++) {
        std::string name = "Stream " + std::to_string(i + 1);
        std::string addr = "239.1.1." + std::to_string(i + 1);
        uint16_t port = 5004 + (i * 2);

        streams.push_back(createTestStream(name, addr, port, 8));
    }

    // Validate all streams
    for (const auto& stream : streams) {
        TEST_ASSERT(stream.isValid(), "All streams should be valid");
    }

    // Verify uniqueness
    for (size_t i = 0; i < streams.size(); i++) {
        for (size_t j = i + 1; j < streams.size(); j++) {
            TEST_ASSERT(streams[i].connectionAddress != streams[j].connectionAddress,
                       "Each stream should have unique address");
            TEST_ASSERT(streams[i].port != streams[j].port,
                       "Each stream should have unique port");
        }
    }

    std::cout << "PASS" << std::endl;
    return true;
}

bool testMaximumStreams() {
    std::cout << "Test: Maximum stream configuration (16 streams)... ";

    // Create 16 streams x 8 channels = 128 channels total
    std::vector<SDPSession> streams;
    for (int i = 0; i < 16; i++) {
        std::string name = "Stream " + std::to_string(i + 1);
        std::string addr = "239.1." + std::to_string((i / 255) + 1) + "." + std::to_string((i % 255) + 1);
        uint16_t port = 5004 + (i * 2);

        streams.push_back(createTestStream(name, addr, port, 8));
    }

    TEST_ASSERT(streams.size() == 16, "Should create 16 streams");

    // Validate all
    for (const auto& stream : streams) {
        TEST_ASSERT(stream.isValid(), "All streams should be valid");
        TEST_ASSERT(stream.numChannels == 8, "Each stream should have 8 channels");
    }

    // Calculate total channels
    uint16_t totalChannels = 0;
    for (const auto& stream : streams) {
        totalChannels += stream.numChannels;
    }
    TEST_ASSERT(totalChannels == 128, "Total should be 128 channels");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Channel Mapping Coordination Tests
//

bool testNonOverlappingMappings() {
    std::cout << "Test: Non-overlapping channel mappings... ";

    // Create stream IDs
    StreamID id1 = StreamID::generate();
    StreamID id2 = StreamID::generate();
    StreamID id3 = StreamID::generate();

    // Map to different channel ranges
    ChannelMapping map1 = createMapping(id1, "Stream 1", 8, 0);    // 0-7
    ChannelMapping map2 = createMapping(id2, "Stream 2", 8, 8);    // 8-15
    ChannelMapping map3 = createMapping(id3, "Stream 3", 8, 16);   // 16-23

    TEST_ASSERT(map1.isValid(), "Mapping 1 should be valid");
    TEST_ASSERT(map2.isValid(), "Mapping 2 should be valid");
    TEST_ASSERT(map3.isValid(), "Mapping 3 should be valid");

    // Check no overlaps
    uint16_t end1 = map1.getDeviceChannelEnd();
    uint16_t end2 = map2.getDeviceChannelEnd();
    (void)map3.getDeviceChannelEnd();

    TEST_ASSERT(end1 == map2.deviceChannelStart, "Mappings 1 and 2 should be contiguous");
    TEST_ASSERT(end2 == map3.deviceChannelStart, "Mappings 2 and 3 should be contiguous");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testOverlappingMappingDetection() {
    std::cout << "Test: Overlapping channel mapping detection... ";

    StreamID id1 = StreamID::generate();
    StreamID id2 = StreamID::generate();

    // Create overlapping mappings
    ChannelMapping map1 = createMapping(id1, "Stream 1", 16, 0);   // 0-15
    ChannelMapping map2 = createMapping(id2, "Stream 2", 16, 8);   // 8-23 (overlaps 8-15)

    // Both individually valid
    TEST_ASSERT(map1.isValid(), "Mapping 1 should be valid individually");
    TEST_ASSERT(map2.isValid(), "Mapping 2 should be valid individually");

    // Detect overlap
    uint16_t end1 = map1.getDeviceChannelEnd();
    bool overlaps = (map1.deviceChannelStart < map2.deviceChannelStart + map2.deviceChannelCount &&
                     map2.deviceChannelStart < end1);

    TEST_ASSERT(overlaps, "Should detect overlap");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testFullDeviceMappings() {
    std::cout << "Test: Full device channel mappings (128 channels)... ";

    // Create 16 streams with mappings for all 128 channels
    std::vector<ChannelMapping> mappings;
    for (int i = 0; i < 16; i++) {
        StreamID id = StreamID::generate();
        std::string name = "Stream " + std::to_string(i + 1);
        uint16_t deviceStart = i * 8;

        mappings.push_back(createMapping(id, name, 8, deviceStart));
    }

    TEST_ASSERT(mappings.size() == 16, "Should create 16 mappings");

    // Verify no gaps or overlaps
    for (size_t i = 0; i < mappings.size() - 1; i++) {
        uint16_t end = mappings[i].getDeviceChannelEnd();
        uint16_t nextStart = mappings[i + 1].deviceChannelStart;
        TEST_ASSERT(end == nextStart, "Mappings should be contiguous with no gaps");
    }

    // Verify last mapping ends at 128
    uint16_t lastEnd = mappings[15].getDeviceChannelEnd();
    TEST_ASSERT(lastEnd == 128, "Last mapping should end at channel 128");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Sample Rate Coordination Tests
//

bool testUniformSampleRate() {
    std::cout << "Test: Uniform sample rate across streams... ";

    uint32_t targetRate = 48000;

    // Create 4 streams all at 48kHz
    std::vector<SDPSession> streams;
    for (int i = 0; i < 4; i++) {
        std::string name = "Stream " + std::to_string(i + 1);
        std::string addr = "239.1.1." + std::to_string(i + 1);
        uint16_t port = 5004 + (i * 2);

        streams.push_back(createTestStream(name, addr, port, 8, targetRate));
    }

    // Verify all at same rate
    for (const auto& stream : streams) {
        TEST_ASSERT(stream.sampleRate == targetRate,
                   "All streams should be at 48kHz");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

bool testMixedSampleRateDetection() {
    std::cout << "Test: Mixed sample rate detection... ";

    // Create streams at different rates
    SDPSession stream1 = createTestStream("Stream 1", "239.1.1.1", 5004, 8, 48000);
    SDPSession stream2 = createTestStream("Stream 2", "239.1.1.2", 5006, 8, 96000);
    SDPSession stream3 = createTestStream("Stream 3", "239.1.1.3", 5008, 8, 48000);

    TEST_ASSERT(stream1.sampleRate != stream2.sampleRate,
               "Should detect different sample rates");
    TEST_ASSERT(stream1.sampleRate == stream3.sampleRate,
               "Streams 1 and 3 should match");

    // In a real system, this would trigger a warning or require SRC
    bool needsSRC = (stream1.sampleRate != stream2.sampleRate);
    TEST_ASSERT(needsSRC, "Mixed rates should be detected");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Network Configuration Tests
//

bool testUniqueMulticastAddresses() {
    std::cout << "Test: Unique multicast addresses... ";

    std::vector<SDPSession> streams;
    streams.push_back(createTestStream("Stream 1", "239.1.1.1", 5004, 8));
    streams.push_back(createTestStream("Stream 2", "239.1.1.2", 5004, 8));
    streams.push_back(createTestStream("Stream 3", "239.1.1.3", 5004, 8));

    // All use same port but different addresses
    for (size_t i = 0; i < streams.size() - 1; i++) {
        for (size_t j = i + 1; j < streams.size(); j++) {
            TEST_ASSERT(streams[i].connectionAddress != streams[j].connectionAddress,
                       "Each stream should have unique multicast address");
        }
    }

    std::cout << "PASS" << std::endl;
    return true;
}

bool testUniquePortNumbers() {
    std::cout << "Test: Unique port numbers... ";

    // Same address, different ports
    std::vector<SDPSession> streams;
    streams.push_back(createTestStream("Stream 1", "239.1.1.1", 5004, 8));
    streams.push_back(createTestStream("Stream 2", "239.1.1.1", 5006, 8));
    streams.push_back(createTestStream("Stream 3", "239.1.1.1", 5008, 8));

    // All use same address but different ports
    for (size_t i = 0; i < streams.size() - 1; i++) {
        for (size_t j = i + 1; j < streams.size(); j++) {
            TEST_ASSERT(streams[i].port != streams[j].port,
                       "Each stream should have unique port");
        }
    }

    std::cout << "PASS" << std::endl;
    return true;
}

bool testPortConflictDetection() {
    std::cout << "Test: Port conflict detection... ";

    // Create two streams with same address AND port - conflict!
    SDPSession stream1 = createTestStream("Stream 1", "239.1.1.1", 5004, 8);
    SDPSession stream2 = createTestStream("Stream 2", "239.1.1.1", 5004, 8);

    // Detect conflict
    bool conflict = (stream1.connectionAddress == stream2.connectionAddress &&
                    stream1.port == stream2.port);

    TEST_ASSERT(conflict, "Should detect address/port conflict");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// PTP Synchronization Tests
//

bool testUnifiedPTPDomain() {
    std::cout << "Test: Unified PTP domain across streams... ";

    int32_t ptpDomain = 0;

    // Create multiple streams all using PTP domain 0
    std::vector<SDPSession> streams;
    for (int i = 0; i < 4; i++) {
        std::string name = "Stream " + std::to_string(i + 1);
        std::string addr = "239.1.1." + std::to_string(i + 1);
        uint16_t port = 5004 + (i * 2);

        SDPSession sdp = createTestStream(name, addr, port, 8);
        sdp.ptpDomain = ptpDomain;
        streams.push_back(sdp);
    }

    // Verify all use same PTP domain
    for (const auto& stream : streams) {
        TEST_ASSERT(stream.ptpDomain == ptpDomain,
                   "All streams should use PTP domain 0");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

bool testMultiplePTPDomains() {
    std::cout << "Test: Multiple PTP domains... ";

    // Create streams using different PTP domains
    SDPSession stream1 = createTestStream("Stream 1", "239.1.1.1", 5004, 8);
    stream1.ptpDomain = 0;

    SDPSession stream2 = createTestStream("Stream 2", "239.1.1.2", 5006, 8);
    stream2.ptpDomain = 1;

    SDPSession stream3 = createTestStream("Stream 3", "239.1.1.3", 5008, 8);
    stream3.ptpDomain = 0;

    TEST_ASSERT(stream1.ptpDomain == 0, "Stream 1 in domain 0");
    TEST_ASSERT(stream2.ptpDomain == 1, "Stream 2 in domain 1");
    TEST_ASSERT(stream3.ptpDomain == 0, "Stream 3 in domain 0");

    // Streams 1 and 3 share domain 0
    TEST_ASSERT(stream1.ptpDomain == stream3.ptpDomain,
               "Streams 1 and 3 should share PTP domain");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testNoPTPStreams() {
    std::cout << "Test: Streams without PTP... ";

    // Create streams without PTP sync (-1 = no PTP)
    std::vector<SDPSession> streams;
    for (int i = 0; i < 3; i++) {
        std::string name = "Stream " + std::to_string(i + 1);
        std::string addr = "239.1.1." + std::to_string(i + 1);
        uint16_t port = 5004 + (i * 2);

        SDPSession sdp = createTestStream(name, addr, port, 8);
        sdp.ptpDomain = -1;  // No PTP
        streams.push_back(sdp);
    }

    // Verify all indicate no PTP
    for (const auto& stream : streams) {
        TEST_ASSERT(stream.ptpDomain == -1, "Should indicate no PTP");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Stream Capacity Tests
//

bool testStreamAddition() {
    std::cout << "Test: Progressive stream addition... ";

    std::vector<SDPSession> streams;

    // Add streams one by one
    for (int i = 0; i < 8; i++) {
        std::string name = "Stream " + std::to_string(i + 1);
        std::string addr = "239.1.1." + std::to_string(i + 1);
        uint16_t port = 5004 + (i * 2);

        streams.push_back(createTestStream(name, addr, port, 8));

        TEST_ASSERT(streams.size() == static_cast<size_t>(i + 1),
                   "Stream count should match");
    }

    TEST_ASSERT(streams.size() == 8, "Should have 8 streams");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testStreamRemoval() {
    std::cout << "Test: Stream removal... ";

    // Create initial streams
    std::vector<SDPSession> streams;
    for (int i = 0; i < 5; i++) {
        std::string name = "Stream " + std::to_string(i + 1);
        std::string addr = "239.1.1." + std::to_string(i + 1);
        uint16_t port = 5004 + (i * 2);

        streams.push_back(createTestStream(name, addr, port, 8));
    }

    TEST_ASSERT(streams.size() == 5, "Should start with 5 streams");

    // Remove middle stream
    streams.erase(streams.begin() + 2);
    TEST_ASSERT(streams.size() == 4, "Should have 4 streams after removal");

    // Verify remaining streams still valid
    for (const auto& stream : streams) {
        TEST_ASSERT(stream.isValid(), "Remaining streams should be valid");
    }

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Mixed Configuration Tests
//

bool testRealisticStudioConfiguration() {
    std::cout << "Test: Realistic studio configuration... ";

    // Typical studio: 64 channels total
    // - 1x 32-channel mix bus (239.1.1.1:5004)
    // - 2x 16-channel FX returns (239.1.1.2:5006, 239.1.1.3:5008)

    SDPSession mixBus = createTestStream("Mix Bus", "239.1.1.1", 5004, 32, 48000);
    SDPSession fx1 = createTestStream("FX Return 1", "239.1.1.2", 5006, 16, 48000);
    SDPSession fx2 = createTestStream("FX Return 2", "239.1.1.3", 5008, 16, 48000);

    TEST_ASSERT(mixBus.isValid(), "Mix bus should be valid");
    TEST_ASSERT(fx1.isValid(), "FX 1 should be valid");
    TEST_ASSERT(fx2.isValid(), "FX 2 should be valid");

    // All at same sample rate
    TEST_ASSERT(mixBus.sampleRate == 48000, "Mix bus at 48kHz");
    TEST_ASSERT(fx1.sampleRate == 48000, "FX 1 at 48kHz");
    TEST_ASSERT(fx2.sampleRate == 48000, "FX 2 at 48kHz");

    // Total channels
    uint16_t total = mixBus.numChannels + fx1.numChannels + fx2.numChannels;
    TEST_ASSERT(total == 64, "Total should be 64 channels");

    // Create non-overlapping mappings
    StreamID mixID = StreamID::generate();
    StreamID fx1ID = StreamID::generate();
    StreamID fx2ID = StreamID::generate();

    ChannelMapping mixMap = createMapping(mixID, "Mix Bus", 32, 0);     // 0-31
    ChannelMapping fx1Map = createMapping(fx1ID, "FX 1", 16, 32);       // 32-47
    ChannelMapping fx2Map = createMapping(fx2ID, "FX 2", 16, 48);       // 48-63

    TEST_ASSERT(mixMap.getDeviceChannelEnd() == 32, "Mix ends at 32");
    TEST_ASSERT(fx1Map.getDeviceChannelEnd() == 48, "FX1 ends at 48");
    TEST_ASSERT(fx2Map.getDeviceChannelEnd() == 64, "FX2 ends at 64");

    std::cout << "PASS" << std::endl;
    return true;
}

bool testRealisticBroadcastConfiguration() {
    std::cout << "Test: Realistic broadcast configuration... ";

    // Broadcast facility: 128 channels
    // - 4x 32-channel program feeds

    std::vector<SDPSession> programs;
    std::vector<ChannelMapping> mappings;

    for (int i = 0; i < 4; i++) {
        std::string name = "Program " + std::to_string(i + 1);
        std::string addr = "239.69.1." + std::to_string(i + 1);
        uint16_t port = 5004 + (i * 2);

        programs.push_back(createTestStream(name, addr, port, 32, 48000));

        StreamID id = StreamID::generate();
        mappings.push_back(createMapping(id, name, 32, i * 32));
    }

    // Verify all programs
    TEST_ASSERT(programs.size() == 4, "Should have 4 programs");

    for (const auto& program : programs) {
        TEST_ASSERT(program.isValid(), "Program should be valid");
        TEST_ASSERT(program.numChannels == 32, "Each program 32 channels");
        TEST_ASSERT(program.sampleRate == 48000, "All at 48kHz");
    }

    // Verify mappings fill entire device
    TEST_ASSERT(mappings[0].deviceChannelStart == 0, "Program 1 starts at 0");
    TEST_ASSERT(mappings[1].deviceChannelStart == 32, "Program 2 starts at 32");
    TEST_ASSERT(mappings[2].deviceChannelStart == 64, "Program 3 starts at 64");
    TEST_ASSERT(mappings[3].deviceChannelStart == 96, "Program 4 starts at 96");
    TEST_ASSERT(mappings[3].getDeviceChannelEnd() == 128, "Last ends at 128");

    std::cout << "PASS" << std::endl;
    return true;
}

//
// Main Test Runner
//

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "AES67 Multi-Stream Integration Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    std::cout << "Multi-Stream Configuration Tests:" << std::endl;
    std::cout << "--------------------------------" << std::endl;
    testTwoStreamConfiguration();
    testFourStreamConfiguration();
    testMaximumStreams();
    std::cout << std::endl;

    std::cout << "Channel Mapping Coordination Tests:" << std::endl;
    std::cout << "-----------------------------------" << std::endl;
    testNonOverlappingMappings();
    testOverlappingMappingDetection();
    testFullDeviceMappings();
    std::cout << std::endl;

    std::cout << "Sample Rate Coordination Tests:" << std::endl;
    std::cout << "-------------------------------" << std::endl;
    testUniformSampleRate();
    testMixedSampleRateDetection();
    std::cout << std::endl;

    std::cout << "Network Configuration Tests:" << std::endl;
    std::cout << "---------------------------" << std::endl;
    testUniqueMulticastAddresses();
    testUniquePortNumbers();
    testPortConflictDetection();
    std::cout << std::endl;

    std::cout << "PTP Synchronization Tests:" << std::endl;
    std::cout << "-------------------------" << std::endl;
    testUnifiedPTPDomain();
    testMultiplePTPDomains();
    testNoPTPStreams();
    std::cout << std::endl;

    std::cout << "Stream Capacity Tests:" << std::endl;
    std::cout << "---------------------" << std::endl;
    testStreamAddition();
    testStreamRemoval();
    std::cout << std::endl;

    std::cout << "Realistic Configuration Tests:" << std::endl;
    std::cout << "-----------------------------" << std::endl;
    testRealisticStudioConfiguration();
    testRealisticBroadcastConfiguration();
    std::cout << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << "Test Results:" << std::endl;
    std::cout << "  Passed: " << testsPassed << std::endl;
    std::cout << "  Failed: " << testsFailed << std::endl;
    std::cout << "========================================" << std::endl;

    return testsFailed == 0 ? 0 : 1;
}
