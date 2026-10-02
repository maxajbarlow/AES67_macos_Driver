//
// TestCriticalPathRegressions.cpp
// AES67 macOS Driver
// Regression tests for the critical-path defects found in review, kept
// current with the step 2 receive path (placement by RTP timestamp):
//   - Core Audio output never reaching the TX ring buffers (mixing mode)
//   - Device nominal sample rate defaulting to 44.1 kHz / rate changes ignored
//   - TX streams reloading as RX streams
//   - The device clock coming from the device's own MediaClock
//   - Input read by device time minus the link offset, for any buffer size,
//     identically for every client
//   - Receiver following a sender restart (ahead of or behind playout)
//   - Lost packets and outages keeping the stream's place on the timeline
//   - Late packets or a second sender on the group being ignored
//
// Uses a non-aborting CHECK so every failure is reported, and works under NDEBUG.
//

#include "RxTestSupport.h"
#include "../Driver/AES67Device.h"
#include "../Driver/AES67IOHandler.h"
#include "../NetworkEngine/StreamManager.h"
#include "../NetworkEngine/RTP/RTPReceiver.h"
#include "../Driver/AudioThreadPriority.h"
#include "../NetworkEngine/RTSafeStreamInterface.h"
#include "../NetworkEngine/Clock/HostTime.h"
#include "../NetworkEngine/Clock/TimestampedAudioBuffer.h"
#include "../Shared/RingBuffer.hpp"
#include <aspl/Context.hpp>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
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

constexpr size_t kNumChannels = 128;
constexpr size_t kTestRingSize = 4096;

template<size_t... Is>
std::array<SPSCRingBuffer<float>, sizeof...(Is)> makeRingBuffers(size_t size, std::index_sequence<Is...>) {
    return {((void)Is, SPSCRingBuffer<float>(size))...};
}

std::array<SPSCRingBuffer<float>, kNumChannels> makeRingBuffers(size_t size) {
    return makeRingBuffers(size, std::make_index_sequence<kNumChannels>{});
}

// Point the driver's config search at a fresh file containing no streams.
std::string useEmptyConfig(const std::string& name) {
    const char* tmp = std::getenv("TMPDIR");
    std::string path = std::string(tmp ? tmp : "/tmp/") + "aes67_regression_" + name + ".json";
    std::ofstream(path) << "{\n  \"version\": \"1.0\",\n  \"streams\": [\n  ]\n}\n";
    setenv("AES67_CONFIG_PATH", path.c_str(), 1);
    return path;
}

// Multicast listener used to observe what a transmitter actually sends.
class MulticastListener {
public:
    MulticastListener(const char* group, uint16_t port) {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        int reuse = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = inet_addr(group);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
        timeval tv{0, 20000};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    ~MulticastListener() { if (fd_ >= 0) close(fd_); }

    size_t countPackets(std::chrono::milliseconds window) {
        size_t count = 0;
        uint8_t buf[2048];
        const auto deadline = std::chrono::steady_clock::now() + window;
        while (std::chrono::steady_clock::now() < deadline) {
            if (recv(fd_, buf, sizeof(buf), 0) > 0) ++count;
        }
        return count;
    }

private:
    int fd_{-1};
};

SDPSession makeRxSDP(const char* group, uint16_t port, uint16_t channels) {
    SDPSession sdp;
    sdp.sessionName = "Regression RX";
    sdp.connectionAddress = group;
    sdp.port = port;
    sdp.numChannels = channels;
    sdp.sampleRate = 48000;
    sdp.encoding = "L24";
    sdp.payloadType = kPayloadTypeL24;
    sdp.ptime = 1;
    sdp.framecount = kFramesPerPacket;
    return sdp;
}

ChannelMapping makeMapping(uint16_t channels, uint16_t deviceStart) {
    ChannelMapping mapping;
    mapping.streamID = StreamID::generate();
    mapping.streamName = "Regression";
    mapping.streamChannelCount = channels;
    mapping.deviceChannelStart = deviceStart;
    mapping.deviceChannelCount = channels;
    return mapping;
}

// Runs a receiver against a fresh device context, sends with `send`, and
// returns what the IO thread would have heard on device channel 0.
template<typename SendFn>
std::vector<float> receiveAndListen(const char* group, uint16_t port, SendFn send, RTPReceiver::PlacementStatistics* stats = nullptr) {
    RxHarness harness;
    RTPReceiver receiver(makeRxSDP(group, port, 2), makeMapping(2, 0), harness.context());
    CHECK(receiver.start(), "receiver should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    PlayoutReader reader(harness, 0);
    LoopbackSender sender(group, port);
    // Pace like a hardware sender: a starved test thread would send packets
    // after their timestamps, which the receiver rightly drops as late
    AudioThreadPriority::configureForRealTime();
    send(sender);
    AudioThreadPriority::restoreNormalPriority();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let playout pass the last packet
    auto samples = reader.stop();
    if (stats) *stats = receiver.getPlacementStatistics();
    receiver.stop();
    return samples;
}

// Sends `count` packets of `value` at 1 ms pacing, skipping indices for which skip(i) is true.
template<typename SkipFn>
void sendRun(LoopbackSender& sender, std::chrono::steady_clock::time_point& next, uint16_t firstSeq,
             uint32_t firstTimestamp, uint16_t count, float value, uint32_t ssrc, SkipFn skip) {
    for (uint16_t i = 0; i < count; ++i) {
        if (!skip(i)) {
            sender.sendL24(static_cast<uint16_t>(firstSeq + i), firstTimestamp + i * kFramesPerPacket, 2, value, ssrc);
        }
        std::this_thread::sleep_until(next += std::chrono::milliseconds(1));
    }
}

const auto kNoSkip = [](uint16_t) { return false; };

// RT-safe interface over a harness's receive context plus TX ring buffers
struct IOFixture {
    RxHarness harness;
    std::array<SPSCRingBuffer<float>, kNumChannels> outputBuffers = makeRingBuffers(8192);
    std::atomic<uint64_t> inputUnderruns{0};
    std::atomic<uint64_t> outputOverruns{0};
    std::atomic<bool> ioRunning{true};
    RTSafeStreamInterface rt{harness.routing, harness.clock, harness.linkOffsetFrames, outputBuffers,
                             inputUnderruns, outputOverruns, ioRunning};
    AES67IOHandler handler{rt, kNumChannels, sizeof(float)};
};

// ---------------------------------------------------------------------------
// C1: Core Audio output must reach the TX ring buffers.
// With DeviceParameters::EnableMixing (the default) libASPL delivers output via
// OnWriteMixedOutput only; OnWriteClientOutput is never called.
// ---------------------------------------------------------------------------
void testMixedOutputReachesOutputBuffers() {
    std::cout << "C1: mixed output reaches TX ring buffers" << std::endl;

    IOFixture io;
    constexpr UInt32 kFrames = 64;
    std::vector<float> interleaved(kFrames * kNumChannels);
    for (UInt32 f = 0; f < kFrames; ++f) {
        for (size_t ch = 0; ch < kNumChannels; ++ch) {
            interleaved[f * kNumChannels + ch] = static_cast<float>(ch) * 0.001f + f * 1e-5f;
        }
    }

    aspl::IORequestHandler& base = io.handler;
    base.OnWriteMixedOutput(std::shared_ptr<aspl::Stream>(), 0.0, 0.0, interleaved.data(),
                            static_cast<UInt32>(interleaved.size() * sizeof(float)));

    CHECK(io.outputBuffers[8].available() == kFrames, "channel 8 should receive every mixed frame");
    float channel8[kFrames] = {};
    io.outputBuffers[8].read(channel8, kFrames);
    bool matches = true;
    for (UInt32 f = 0; f < kFrames; ++f) {
        matches = matches && std::fabs(channel8[f] - (8 * 0.001f + f * 1e-5f)) < 1e-7f;
    }
    CHECK(matches, "channel 8 samples should be de-interleaved from the mixed buffer");

    // Output buffers larger than the handler's scratch buffer are processed in chunks
    constexpr UInt32 kLarge = 5000;
    io.outputBuffers[0].reset();
    std::vector<float> large(kLarge * kNumChannels, 0.5f);
    base.OnWriteMixedOutput(std::shared_ptr<aspl::Stream>(), 0.0, 0.0, large.data(),
                            static_cast<UInt32>(large.size() * sizeof(float)));
    CHECK(io.outputBuffers[0].available() == kLarge, "all 5000 output frames should reach the TX ring");
}

// ---------------------------------------------------------------------------
// Input is read by device time: Core Audio's sample time T maps to media
// position origin + T, and the IO thread plays what was received for
// (origin + T - link offset). Reads are non-destructive, so every client
// hears the same audio.
// ---------------------------------------------------------------------------
void testInputReadsByDeviceTime() {
    std::cout << "IO handler reads input by device time minus link offset" << std::endl;

    IOFixture io;
    aspl::IORequestHandler& base = io.handler;
    constexpr UInt32 kFrames = 5000;  // larger than the old 4096-frame limit
    constexpr int64_t kDeviceTime = 100000;
    const int64_t readPosition = io.harness.clock.snapshot().origin + kDeviceTime - kTestLinkOffset;

    TimestampedAudioBuffer stream(2, 8192);
    std::vector<float> ramp(kFrames * 2);
    for (UInt32 f = 0; f < kFrames; ++f) {
        ramp[f * 2] = static_cast<float>(f) * 1e-4f;
        ramp[f * 2 + 1] = -static_cast<float>(f) * 1e-4f;
    }
    stream.write(readPosition, kFrames, ramp.data(), 2, 0);
    CHECK(io.harness.routing.publish(&stream, 6), "route should publish");

    std::vector<float> clientA(kFrames * kNumChannels, -1.0f);
    std::vector<float> clientB(kFrames * kNumChannels, -2.0f);
    const auto bytes = static_cast<UInt32>(clientA.size() * sizeof(float));
    base.OnReadClientInput(std::shared_ptr<aspl::Client>(), std::shared_ptr<aspl::Stream>(), 0.0, kDeviceTime,
                           clientA.data(), bytes);
    base.OnReadClientInput(std::shared_ptr<aspl::Client>(), std::shared_ptr<aspl::Stream>(), 0.0, kDeviceTime,
                           clientB.data(), bytes);

    bool placed = true;
    bool othersSilent = true;
    for (UInt32 f = 0; f < kFrames; ++f) {
        placed = placed && clientA[f * kNumChannels + 6] == ramp[f * 2] && clientA[f * kNumChannels + 7] == ramp[f * 2 + 1];
        othersSilent = othersSilent && clientA[f * kNumChannels + 5] == 0.0f && clientA[f * kNumChannels + 8] == 0.0f &&
                       clientA[f * kNumChannels] == 0.0f;
    }
    CHECK(placed, "the stream should land on device channels 6-7 at device time minus link offset, all 5000 frames");
    CHECK(othersSilent, "channels no stream feeds should be silent");
    CHECK(clientA == clientB, "a second client in the same cycle should read identical input");
    CHECK(io.inputUnderruns.load() == 0, "fully covered reads should not count as underruns");

    std::vector<float> later(512 * kNumChannels, -1.0f);
    base.OnReadClientInput(std::shared_ptr<aspl::Client>(), std::shared_ptr<aspl::Stream>(), 0.0, kDeviceTime + 50000,
                           later.data(), static_cast<UInt32>(later.size() * sizeof(float)));
    CHECK(std::all_of(later.begin(), later.end(), [](float v) { return v == 0.0f; }), "missing audio should read as silence");
    CHECK(io.inputUnderruns.load() == 1, "a read the stream does not cover should count one underrun");
    io.harness.routing.unpublish(&stream);
}

// ---------------------------------------------------------------------------
// C2: device nominal rate must match the stream formats and follow rate changes.
// ---------------------------------------------------------------------------
void testDeviceSampleRate() {
    std::cout << "C2: device nominal sample rate" << std::endl;
    useEmptyConfig("device");

    auto context = std::make_shared<aspl::Context>();
    auto device = std::make_shared<AES67Device>(context);
    device->Initialize();

    CHECK(device->GetNominalSampleRate() == 48000.0, "nominal rate should start at 48 kHz, not libASPL's 44.1 kHz default");
    CHECK(device->GetInputStream()->GetPhysicalFormat().mSampleRate == device->GetNominalSampleRate(),
          "input stream format should match the device nominal rate");

    // The default test streams are 48 kHz, so only 48 kHz can be offered.
    const auto rates = device->GetAvailableSampleRates();
    CHECK(rates.size() == 1 && rates[0].mMinimum == 48000.0,
          "available rates should be limited to what the active streams can deliver");
    device->SetNominalSampleRateAsync(96000.0);
    CHECK(device->GetNominalSampleRate() == 48000.0, "96 kHz should be refused while 48 kHz streams exist");

    // With no streams, a rate change must propagate everywhere.
    device->GetStreamManager()->removeAllStreams();
    device->SetNominalSampleRateAsync(96000.0);
    CHECK(device->GetNominalSampleRate() == 96000.0, "nominal rate should change to 96 kHz");
    CHECK(device->GetSampleRate() == 96000.0, "driver's own rate should follow the nominal rate");
    CHECK(device->GetStreamManager()->getDeviceSampleRate() == 96000.0, "StreamManager should follow the nominal rate");
    CHECK(device->GetInputStream()->GetPhysicalFormat().mSampleRate == 96000.0, "input physical format should follow");
    CHECK(device->GetOutputStream()->GetPhysicalFormat().mSampleRate == 96000.0, "output physical format should follow");
    CHECK(device->GetOutputStream()->GetVirtualFormat().mSampleRate == 96000.0, "output virtual format should follow");
}

// ---------------------------------------------------------------------------
// Phase 1: the device's Core Audio clock comes from its MediaClock.
// ---------------------------------------------------------------------------
void testDeviceClockFromMediaClock() {
    std::cout << "Device clock driven by MediaClock" << std::endl;
    useEmptyConfig("deviceclock");

    auto context = std::make_shared<aspl::Context>();
    auto device = std::make_shared<AES67Device>(context);
    device->Initialize();

    CHECK(device->GetClockAlgorithm() == kAudioDeviceClockAlgorithmRaw,
          "clock algorithm should be Raw: timestamps come from the clock model, not HAL smoothing");
    CHECK(device->GetZeroTimeStampPeriod() == 16384, "zero timestamp period should be 16384 frames");
    CHECK(device->GetClockIsStable(), "the host-clock source should report a stable clock");

    CHECK(device->StartIO(device->GetID(), 0) == kAudioHardwareNoError, "IO should start");
    const double hostTicksPerSecond = HostTimebase::current().ticksPerSecond();
    Float64 sampleTime = -1;
    UInt64 hostTime = 0;
    UInt64 seed = 0;
    const uint64_t before = hostTimeNow();
    CHECK(device->GetZeroTimeStamp(device->GetID(), 0, &sampleTime, &hostTime, &seed) == kAudioHardwareNoError,
          "GetZeroTimeStamp should succeed while running");
    CHECK(std::fmod(sampleTime, 16384.0) == 0.0, "zero timestamp should sit on a period boundary");
    CHECK(hostTime <= before && static_cast<double>(before - hostTime) < 16384.0 / 48000.0 * hostTicksPerSecond,
          "zero timestamp should be the latest boundary before now at 48 kHz");
    CHECK(seed != 0, "seed should be set");

    // The timestamp must come from the device's own MediaClock: its host time
    // is exactly where that sample starts on the clock, and the seed is the
    // clock's generation (libASPL's default clock would match neither)
    const auto clock = device->GetMediaClock().snapshot();
    CHECK(clock.sampleAt(hostTime) - clock.origin == static_cast<int64_t>(sampleTime) &&
              clock.sampleAt(hostTime - 1) - clock.origin == static_cast<int64_t>(sampleTime) - 1,
          "zero timestamp should land exactly on the device's MediaClock");
    CHECK(seed == clock.generation, "seed should be the MediaClock generation");

    // Rate change: new timeline (seed) at the new rate
    device->StopIO(device->GetID(), 0);
    device->GetStreamManager()->removeAllStreams();
    device->SetNominalSampleRateAsync(96000.0);
    device->StartIO(device->GetID(), 0);
    Float64 sampleTime96 = -1;
    UInt64 hostTime96 = 0;
    UInt64 seed96 = 0;
    const uint64_t before96 = hostTimeNow();
    device->GetZeroTimeStamp(device->GetID(), 0, &sampleTime96, &hostTime96, &seed96);
    CHECK(seed96 != seed, "a sample rate change should start a new timeline (seed)");
    CHECK(static_cast<double>(before96 - hostTime96) < 16384.0 / 96000.0 * hostTicksPerSecond,
          "after the change, boundaries should be spaced for 96 kHz");
    device->StopIO(device->GetID(), 0);
}

// ---------------------------------------------------------------------------
// TX streams must reload as transmitters.
// ---------------------------------------------------------------------------
void testTxStreamSurvivesReload() {
    std::cout << "TX stream survives save/reload" << std::endl;
    useEmptyConfig("txreload");

    constexpr const char* kGroup = "239.69.99.2";
    constexpr uint16_t kPort = 55010;
    RxHarness harness;
    auto outputBuffers = makeRingBuffers(kTestRingSize);

    StreamID txID;
    {
        StreamManager manager(harness.context(), outputBuffers);
        txID = manager.createTxStream("Regression TX", kGroup, kPort, 8, makeMapping(8, 8));
        CHECK(!txID.isNull(), "TX stream should be created");
    }

    StreamManager reloaded(harness.context(), outputBuffers);
    CHECK(reloaded.loadSavedStreams(), "saved TX stream should load");

    MulticastListener listener(kGroup, kPort);
    reloaded.setIOActive(true);
    const size_t packets = listener.countPackets(std::chrono::milliseconds(200));
    reloaded.setIOActive(false);
    CHECK(packets > 50, "reloaded TX stream should transmit (saw " << packets << " packets in 200 ms)");
}

// ---------------------------------------------------------------------------
// Receive path (step 2 phase 2): audio is placed by RTP timestamp. Tests
// judge the stream as heard, from its first to its last non-silent sample.
// ---------------------------------------------------------------------------
void testReceiverFollowsSenderRestart() {
    std::cout << "RX follows a sender restart" << std::endl;

    RTPReceiver::PlacementStatistics stats{};
    const auto samples = receiveAndListen("239.69.99.3", 55012, [](LoopbackSender& sender) {
        auto next = std::chrono::steady_clock::now();
        sendRun(sender, next, 1000, 0, 40, 0.25f, 0x12345678, kNoSkip);
        // Sender restarts with a new SSRC and unrelated sequence and timestamp bases
        sendRun(sender, next, 40000, 900000, 200, -0.5f, 0x0BADF00D, kNoSkip);
    }, &stats);

    const size_t restarted = countNear(samples, -0.5f);
    CHECK(restarted >= 150 * kFramesPerPacket,
          "audio from the restarted sender should play promptly (got " << restarted / kFramesPerPacket << " packets)");
    CHECK(stats.reanchors >= 1, "the restart should re-anchor");
}

void testLostPacketKeepsTimeline() {
    std::cout << "RX lost packet leaves exactly its own gap" << std::endl;

    constexpr uint16_t kPackets = 30;
    RTPReceiver::PlacementStatistics stats{};
    const auto region = heardRegion(receiveAndListen("239.69.99.4", 55014, [](LoopbackSender& sender) {
        auto next = std::chrono::steady_clock::now();
        sendRun(sender, next, 2000, 0, kPackets, 0.25f, 0x12345678, [](uint16_t i) { return i == 15; });
    }, &stats));

    CHECK(countNear(region, 0.25f) == (kPackets - 1) * kFramesPerPacket,
          "every received packet should be played (late drops: " << stats.lateDrops << ")");
    CHECK(region.size() == kPackets * kFramesPerPacket,
          "the stream should keep its length (got " << region.size() << " samples, want " << kPackets * kFramesPerPacket << ")");
    CHECK(countNear(region, 0.0f) == kFramesPerPacket, "the lost packet should leave exactly one packet of silence");
}

void testLatePacketIsIgnored() {
    std::cout << "RX ignores a single very late packet" << std::endl;

    constexpr uint16_t kPackets = 60;
    const auto region = heardRegion(receiveAndListen("239.69.99.5", 55016, [](LoopbackSender& sender) {
        auto next = std::chrono::steady_clock::now();
        for (uint16_t i = 0; i < kPackets; ++i) {
            sender.sendL24(static_cast<uint16_t>(3000 + i), 400 * kFramesPerPacket + i * kFramesPerPacket, 2, 0.25f);
            if (i == 30) {
                sender.sendL24(static_cast<uint16_t>(3000 - 400), 0, 2, 0.9f);  // 400 packets late
            }
            std::this_thread::sleep_until(next += std::chrono::milliseconds(1));
        }
    }));

    CHECK(countNear(region, 0.9f) == 0, "the late packet should not be played");
    CHECK(countNear(region, 0.25f) == kPackets * kFramesPerPacket, "every in-order packet should still be played");
    CHECK(countNear(region, 0.0f) == 0, "a late packet should not cause silence");
}

void testInterleavedSecondSourceIsIgnored() {
    std::cout << "RX ignores an interleaved second source" << std::endl;

    constexpr uint16_t kPackets = 100;
    const auto region = heardRegion(receiveAndListen("239.69.99.6", 55018, [](LoopbackSender& sender) {
        auto next = std::chrono::steady_clock::now();
        for (uint16_t i = 0; i < kPackets; ++i) {
            sender.sendL24(static_cast<uint16_t>(5000 + i), i * kFramesPerPacket, 2, 0.25f, 0xAAAA0001);
            sender.sendL24(static_cast<uint16_t>(20000 + i), 777 + i * kFramesPerPacket, 2, 0.9f, 0xBBBB0002);
            std::this_thread::sleep_until(next += std::chrono::milliseconds(1));
        }
    }));

    CHECK(countNear(region, 0.9f) == 0, "the second source should never be played");
    CHECK(countNear(region, 0.25f) == kPackets * kFramesPerPacket,
          "the first source should play uninterrupted (got " << countNear(region, 0.25f) / kFramesPerPacket << " packets)");
}

void runRestartBehindPlayout(const char* group, uint16_t port, uint32_t restartSsrc, const char* label) {
    const auto samples = receiveAndListen(group, port, [restartSsrc](LoopbackSender& sender) {
        auto next = std::chrono::steady_clock::now();
        sendRun(sender, next, 6000, 200 * kFramesPerPacket, 60, 0.25f, 0x12345678, kNoSkip);
        // Restart with timestamps ~150 packets behind where playout has reached
        sendRun(sender, next, 5900, 110 * kFramesPerPacket, 150, -0.5f, restartSsrc, kNoSkip);
    });

    CHECK(countNear(samples, -0.5f) >= 100 * kFramesPerPacket,
          label << ": the restarted stream should play (got " << countNear(samples, -0.5f) / kFramesPerPacket
                << " of 150 packets)");
}

void testRestartBehindPlayoutIsFollowed() {
    std::cout << "RX follows a restart that lands behind playout" << std::endl;
    runRestartBehindPlayout("239.69.99.7", 55020, 0x12345678, "same SSRC");
    runRestartBehindPlayout("239.69.99.8", 55022, 0x0BADF00D, "new SSRC");
}

void testOutageKeepsTimeline() {
    std::cout << "RX outage plays as its own length of silence" << std::endl;

    constexpr uint16_t kOutagePackets = 40;
    RTPReceiver::PlacementStatistics stats{};
    const auto region = heardRegion(receiveAndListen("239.69.99.9", 55024, [](LoopbackSender& sender) {
        auto next = std::chrono::steady_clock::now();
        // Packets 30..69 are lost in a 40 ms outage
        sendRun(sender, next, 7000, 0, 100, 0.25f, 0x12345678,
                [](uint16_t i) { return i >= 30 && i < 30 + kOutagePackets; });
    }, &stats));

    CHECK(countNear(region, 0.25f) == (100 - kOutagePackets) * kFramesPerPacket,
          "every received packet should play (late drops: " << stats.lateDrops << ")");
    CHECK(region.size() == 100 * kFramesPerPacket,
          "the outage must not add latency: the stream should keep its length (got " << region.size() << " samples)");
    CHECK(countNear(region, 0.0f) == kOutagePackets * kFramesPerPacket, "the outage should be exactly its own length of silence");
}


// ---------------------------------------------------------------------------
// kAudioStreamPropertyIsActive belongs to the HAL: it activates the streams
// its clients use. The device must not change it on IO start/stop, because
// libASPL's Stream::SetIsActive() sends a property-change notification, and
// AudioServerPlugIn.h forbids devices from notifying changes that affect IO.
// ---------------------------------------------------------------------------
void testIOStartStopLeavesStreamActivityToHAL() {
    std::cout << "IO start/stop leaves stream activity to the HAL" << std::endl;
    useEmptyConfig("streamactive");

    auto context = std::make_shared<aspl::Context>();
    auto device = std::make_shared<AES67Device>(context);
    device->Initialize();

    // The HAL deactivates a stream no client uses (here: the output)
    device->GetOutputStream()->SetIsActive(false);

    device->StartIO(device->GetID(), 0);
    CHECK(!device->GetOutputStream()->GetIsActive(), "starting IO must not reactivate a stream the HAL deactivated");
    CHECK(device->GetInputStream()->GetIsActive(), "starting IO must leave the input stream as the HAL set it");

    device->StopIO(device->GetID(), 0);
    CHECK(device->GetInputStream()->GetIsActive(), "stopping IO must not deactivate streams (the HAL owns that property)");
}

} // namespace

int main() {
    testMixedOutputReachesOutputBuffers();
    testInputReadsByDeviceTime();
    testDeviceSampleRate();
    testDeviceClockFromMediaClock();
    testIOStartStopLeavesStreamActivityToHAL();
    testTxStreamSurvivesReload();
    testReceiverFollowsSenderRestart();
    testLostPacketKeepsTimeline();
    testLatePacketIsIgnored();
    testInterleavedSecondSourceIsIgnored();
    testRestartBehindPlayoutIsFollowed();
    testOutageKeepsTimeline();

    std::cout << "\nCritical path regressions: " << checksPassed << " passed, "
              << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
