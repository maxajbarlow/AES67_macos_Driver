//
// TestCriticalPathRegressions.cpp
// AES67 macOS Driver
// Regression tests for the critical-path defects found in review:
//   - Core Audio output never reaching the TX ring buffers (mixing mode)
//   - Device nominal sample rate defaulting to 44.1 kHz / rate changes ignored
//   - Receive rate controller regulating the wrong buffer and running to its clamp
//   - TX streams reloading as RX streams
//   - Stale jitter buffer slots blocking newer packets
//   - Receiver not resyncing after a sender restart
//   - Lost packets shifting the timeline instead of being replaced by silence
//   - Late packets or a second sender on the group triggering spurious resyncs
//   - A restart landing behind the playout point leaving the stream silent
//   - Outages being replayed as silence on top of the underrun, adding latency
//
// Uses a non-aborting CHECK so every failure is reported, and works under NDEBUG.
//

#include "../Driver/AES67Device.h"
#include "../Driver/AES67IOHandler.h"
#include "../NetworkEngine/StreamManager.h"
#include "../NetworkEngine/RTP/RTPReceiver.h"
#include "../NetworkEngine/RTP/RateController.h"
#include "../NetworkEngine/RTSafeStreamInterface.h"
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
constexpr uint8_t kPayloadTypeL24 = 97;
constexpr uint32_t kFramesPerPacket = 48;

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

// Multicast UDP socket that only delivers to this host (TTL 0).
class LoopbackSender {
public:
    LoopbackSender(const char* group, uint16_t port) {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        unsigned char ttl = 0;
        unsigned char loop = 1;
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        dest_.sin_family = AF_INET;
        dest_.sin_port = htons(port);
        dest_.sin_addr.s_addr = inet_addr(group);
    }
    ~LoopbackSender() { if (fd_ >= 0) close(fd_); }

    // Send one L24 packet where every sample of every channel equals `value`.
    void sendL24(uint16_t seq, uint32_t timestamp, uint16_t channels, float value,
                 uint32_t ssrc = 0x12345678) {
        std::vector<uint8_t> pkt(12 + kFramesPerPacket * channels * 3);
        pkt[0] = 0x80;
        pkt[1] = kPayloadTypeL24;
        pkt[2] = seq >> 8;
        pkt[3] = seq & 0xFF;
        for (int i = 0; i < 4; ++i) pkt[4 + i] = (timestamp >> (24 - 8 * i)) & 0xFF;
        for (int i = 0; i < 4; ++i) pkt[8 + i] = (ssrc >> (24 - 8 * i)) & 0xFF;
        const int32_t pcm = static_cast<int32_t>(value * 8388607.0f);
        for (size_t s = 0; s < kFramesPerPacket * channels; ++s) {
            pkt[12 + s * 3 + 0] = (pcm >> 16) & 0xFF;
            pkt[12 + s * 3 + 1] = (pcm >> 8) & 0xFF;
            pkt[12 + s * 3 + 2] = pcm & 0xFF;
        }
        sendto(fd_, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&dest_), sizeof(dest_));
    }

private:
    int fd_{-1};
    sockaddr_in dest_{};
};

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

// Drains one device channel on a background thread, like Core Audio would.
class ChannelDrainer {
public:
    explicit ChannelDrainer(SPSCRingBuffer<float>& ring) : ring_(ring) {
        thread_ = std::thread([this] {
            float chunk[512];
            while (running_) {
                const size_t n = ring_.read(chunk, 512);
                samples_.insert(samples_.end(), chunk, chunk + n);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    }
    std::vector<float> stop() {
        running_ = false;
        thread_.join();
        float chunk[512];
        size_t n;
        while ((n = ring_.read(chunk, 512)) > 0) samples_.insert(samples_.end(), chunk, chunk + n);
        return samples_;
    }

private:
    SPSCRingBuffer<float>& ring_;
    std::atomic<bool> running_{true};
    std::vector<float> samples_;
    std::thread thread_;
};

size_t countNear(const std::vector<float>& samples, float value) {
    return std::count_if(samples.begin(), samples.end(),
                         [value](float s) { return std::fabs(s - value) < 1e-3f; });
}

// ---------------------------------------------------------------------------
// C1: Core Audio output must reach the TX ring buffers.
// With DeviceParameters::EnableMixing (the default) libASPL delivers output via
// OnWriteMixedOutput only; OnWriteClientOutput is never called.
// ---------------------------------------------------------------------------
void testMixedOutputReachesOutputBuffers() {
    std::cout << "C1: mixed output reaches TX ring buffers" << std::endl;

    auto inputBuffers = makeRingBuffers(kTestRingSize);
    auto outputBuffers = makeRingBuffers(kTestRingSize);
    std::atomic<uint64_t> inputUnderruns{0};
    std::atomic<uint64_t> outputOverruns{0};
    std::atomic<bool> ioRunning{true};
    RTSafeStreamInterface rt(inputBuffers, outputBuffers, inputUnderruns, outputOverruns, ioRunning);
    AES67IOHandler handler(rt, kNumChannels, sizeof(float));

    constexpr UInt32 kFrames = 64;
    std::vector<float> interleaved(kFrames * kNumChannels);
    for (UInt32 f = 0; f < kFrames; ++f) {
        for (size_t ch = 0; ch < kNumChannels; ++ch) {
            interleaved[f * kNumChannels + ch] = static_cast<float>(ch) * 0.001f + f * 1e-5f;
        }
    }

    aspl::IORequestHandler& base = handler;
    base.OnWriteMixedOutput(std::shared_ptr<aspl::Stream>(), 0.0, 0.0, interleaved.data(),
                            static_cast<UInt32>(interleaved.size() * sizeof(float)));

    CHECK(outputBuffers[8].available() == kFrames, "channel 8 should receive every mixed frame");
    float channel8[kFrames] = {};
    outputBuffers[8].read(channel8, kFrames);
    bool matches = true;
    for (UInt32 f = 0; f < kFrames; ++f) {
        matches = matches && std::fabs(channel8[f] - (8 * 0.001f + f * 1e-5f)) < 1e-7f;
    }
    CHECK(matches, "channel 8 samples should be de-interleaved from the mixed buffer");
}

// IO buffers larger than the handler's 4096-frame scratch buffer must be
// processed in chunks, not silently dropped (output) or zeroed (input).
void testLargeIOBuffersAreChunked() {
    std::cout << "IO handler chunks buffers over 4096 frames" << std::endl;

    constexpr UInt32 kFrames = 5000;
    auto inputBuffers = makeRingBuffers(8192);
    auto outputBuffers = makeRingBuffers(8192);
    std::atomic<uint64_t> inputUnderruns{0};
    std::atomic<uint64_t> outputOverruns{0};
    std::atomic<bool> ioRunning{true};
    RTSafeStreamInterface rt(inputBuffers, outputBuffers, inputUnderruns, outputOverruns, ioRunning);
    AES67IOHandler handler(rt, kNumChannels, sizeof(float));
    aspl::IORequestHandler& base = handler;

    std::vector<float> interleaved(kFrames * kNumChannels, 0.5f);
    base.OnWriteMixedOutput(std::shared_ptr<aspl::Stream>(), 0.0, 0.0, interleaved.data(),
                            static_cast<UInt32>(interleaved.size() * sizeof(float)));
    CHECK(outputBuffers[0].available() == kFrames, "all 5000 output frames should reach the TX ring");

    std::vector<float> ramp(kFrames);
    for (UInt32 f = 0; f < kFrames; ++f) ramp[f] = f * 1e-4f;
    inputBuffers[3].write(ramp.data(), kFrames);
    std::vector<float> clientBuffer(kFrames * kNumChannels, -1.0f);
    base.OnReadClientInput(std::shared_ptr<aspl::Client>(), std::shared_ptr<aspl::Stream>(), 0.0, 0.0,
                           clientBuffer.data(), static_cast<UInt32>(clientBuffer.size() * sizeof(float)));
    CHECK(clientBuffer[(kFrames - 1) * kNumChannels + 3] == ramp[kFrames - 1],
          "the last of 5000 input frames should be delivered, not zeroed");
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
// C3: the consume thread must lock to the sender's packet rate, holding the
// jitter buffer at its prefill depth whatever the sender's clock offset.
// (The original loop measured the device ring buffer instead, which the
// consume thread cannot regulate, and ran itself to the clamp.)
// Event simulation: packets arrive at 1 kHz * (1 + drift); the consumer ticks
// at controller-paced intervals taking one packet if one is buffered.
// ---------------------------------------------------------------------------
void simulateRateController(double senderDrift) {
    constexpr double kDuration = 180.0;
    constexpr double kSettleTime = 120.0;
    constexpr size_t kTargetDepth = 6;
    const std::chrono::microseconds kTick(1000);

    RateController controller(kTick, kTargetDepth);
    double depth = kTargetDepth;  // consumption starts once prefill completes
    double nextArrival = 0.0;
    double nextTick = 0.0;
    const double arrivalPeriod = 0.001 / (1.0 + senderDrift);
    size_t emptyTicksAfterSettle = 0;
    double minDepth = 1e9;
    double maxDepth = 0.0;

    while (std::min(nextArrival, nextTick) < kDuration) {
        if (nextArrival <= nextTick) {
            depth += 1.0;
            nextArrival += arrivalPeriod;
            continue;
        }
        if (depth >= 1.0) {
            depth -= 1.0;
        } else if (nextTick > kSettleTime) {
            ++emptyTicksAfterSettle;
        }
        controller.addDepthSample(depth);
        if (nextTick > kSettleTime) {
            minDepth = std::min(minDepth, depth);
            maxDepth = std::max(maxDepth, depth);
        }
        nextTick += std::chrono::duration<double>(controller.nextInterval(kTick)).count();
    }

    std::ostringstream label;
    label << "sender drift " << senderDrift * 1e6 << " ppm";
    CHECK(emptyTicksAfterSettle == 0, label.str() << ": consumer should never find the jitter buffer empty once settled (got "
                                                  << emptyTicksAfterSettle << ")");
    CHECK(minDepth >= 2.0 && maxDepth <= 12.0,
          label.str() << ": jitter buffer depth should stay near " << kTargetDepth
                      << " (range " << minDepth << ".." << maxDepth << ")");
}

void testRateControllerTracksSender() {
    std::cout << "C3: rate controller tracks the sender's packet rate" << std::endl;
    simulateRateController(0.0);
    simulateRateController(+100e-6);
    simulateRateController(-100e-6);
    simulateRateController(+1000e-6);
}

// ---------------------------------------------------------------------------
// TX streams must reload as transmitters.
// ---------------------------------------------------------------------------
void testTxStreamSurvivesReload() {
    std::cout << "TX stream survives save/reload" << std::endl;
    useEmptyConfig("txreload");

    constexpr const char* kGroup = "239.69.99.2";
    constexpr uint16_t kPort = 55010;
    auto inputBuffers = makeRingBuffers(kTestRingSize);
    auto outputBuffers = makeRingBuffers(kTestRingSize);

    StreamID txID;
    {
        StreamManager manager(inputBuffers, outputBuffers);
        txID = manager.createTxStream("Regression TX", kGroup, kPort, 8, makeMapping(8, 8));
        CHECK(!txID.isNull(), "TX stream should be created");
    }

    StreamManager reloaded(inputBuffers, outputBuffers);
    CHECK(reloaded.loadSavedStreams(), "saved TX stream should load");

    MulticastListener listener(kGroup, kPort);
    reloaded.setIOActive(true);
    const size_t packets = listener.countPackets(std::chrono::milliseconds(200));
    reloaded.setIOActive(false);
    CHECK(packets > 50, "reloaded TX stream should transmit (saw " << packets << " packets in 200 ms)");
}

// ---------------------------------------------------------------------------
// A stale packet left in a jitter buffer slot must not block a newer packet
// that maps to the same slot, but a late packet must not evict a newer one.
// ---------------------------------------------------------------------------
void testJitterBufferReplacesStaleSlot() {
    std::cout << "Jitter buffer replaces stale slots" << std::endl;

    LockFreeCircularJitterBuffer buffer(256);
    const uint8_t oldPayload[4] = {1, 1, 1, 1};
    const uint8_t newPayload[4] = {2, 2, 2, 2};
    uint8_t out[16];
    size_t outLength = 0;
    uint64_t presentation = 0;

    CHECK(buffer.addPacket(oldPayload, 4, 10, 0), "first packet should be stored");
    CHECK(buffer.addPacket(newPayload, 4, 266, 0), "newer packet in the same slot should replace the stale one");
    CHECK(buffer.getBufferedPacketCount() == 1, "replacement should not change the buffered count");
    CHECK(buffer.getNextPacket(out, sizeof(out), outLength, presentation, 266) && out[0] == 2,
          "the newer packet should be readable");

    CHECK(buffer.addPacket(newPayload, 4, 65535, 0), "packet before wraparound should be stored");
    CHECK(!buffer.addPacket(oldPayload, 4, 65535 - 256, 0), "a late packet must not evict a newer one");
    CHECK(buffer.addPacket(oldPayload, 4, 255, 0), "packet after 16-bit wraparound counts as newer");
    CHECK(buffer.getNextPacket(out, sizeof(out), outLength, presentation, 255) && out[0] == 1,
          "the post-wraparound packet should be readable");

    CHECK(RTP::sequenceDistance(65535, 1) == 2, "distance should be forward across wraparound");
    CHECK(RTP::sequenceDistance(1, 65535) == -2, "distance should be backward across wraparound");
}

// ---------------------------------------------------------------------------
// Receiver must resync when the sender restarts with a new sequence base.
// ---------------------------------------------------------------------------
void testReceiverResyncsAfterSenderRestart() {
    std::cout << "RX resyncs after sender restart" << std::endl;

    constexpr const char* kGroup = "239.69.99.3";
    constexpr uint16_t kPort = 55012;
    auto deviceBuffers = makeRingBuffers(kTestRingSize);
    RTPReceiver receiver(makeRxSDP(kGroup, kPort, 2), makeMapping(2, 0), deviceBuffers);
    CHECK(receiver.start(), "receiver should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ChannelDrainer drainer(deviceBuffers[0]);
    LoopbackSender sender(kGroup, kPort);
    auto next = std::chrono::steady_clock::now();
    for (uint16_t i = 0; i < 40; ++i) {
        sender.sendL24(static_cast<uint16_t>(1000 + i), i * kFramesPerPacket, 2, 0.25f);
        std::this_thread::sleep_until(next += std::chrono::milliseconds(1));
    }
    // Sender restarts with a new random sequence and timestamp base.
    for (uint16_t i = 0; i < 200; ++i) {
        sender.sendL24(static_cast<uint16_t>(40000 + i), 900000 + i * kFramesPerPacket, 2, -0.5f, 0x0BADF00D);
        std::this_thread::sleep_until(next += std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    receiver.stop();
    const auto samples = drainer.stop();

    const size_t restarted = countNear(samples, -0.5f);
    CHECK(restarted >= 150 * kFramesPerPacket,
          "audio from the restarted sender should play promptly (got " << restarted / kFramesPerPacket << " packets)");
}

// ---------------------------------------------------------------------------
// A lost packet must be replaced with one packet of silence so the stream
// keeps its position on the timeline.
// ---------------------------------------------------------------------------
void testLostPacketBecomesSilence() {
    std::cout << "RX lost packet becomes silence" << std::endl;

    constexpr const char* kGroup = "239.69.99.4";
    constexpr uint16_t kPort = 55014;
    auto deviceBuffers = makeRingBuffers(kTestRingSize);
    RTPReceiver receiver(makeRxSDP(kGroup, kPort, 2), makeMapping(2, 0), deviceBuffers);
    CHECK(receiver.start(), "receiver should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ChannelDrainer drainer(deviceBuffers[0]);
    LoopbackSender sender(kGroup, kPort);
    constexpr uint16_t kPackets = 30;
    constexpr uint16_t kDropped = 15;
    auto next = std::chrono::steady_clock::now();
    for (uint16_t i = 0; i < kPackets; ++i) {
        if (i != kDropped) {
            sender.sendL24(static_cast<uint16_t>(2000 + i), i * kFramesPerPacket, 2, 0.25f);
        }
        std::this_thread::sleep_until(next += std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    receiver.stop();
    const auto samples = drainer.stop();

    CHECK(countNear(samples, 0.25f) == (kPackets - 1) * kFramesPerPacket, "every received packet should be played");
    CHECK(samples.size() == kPackets * kFramesPerPacket,
          "the lost packet should be filled so the stream keeps its length (got "
              << samples.size() << " samples, want " << kPackets * kFramesPerPacket << ")");
    CHECK(countNear(samples, 0.0f) == kFramesPerPacket, "the filler should be exactly one packet of silence");
}

// ---------------------------------------------------------------------------
// A single packet far behind the playout point (very late or duplicated) must
// be ignored, not treated as a sender restart.
// ---------------------------------------------------------------------------
void testLatePacketDoesNotResync() {
    std::cout << "RX ignores a single very late packet" << std::endl;

    constexpr const char* kGroup = "239.69.99.5";
    constexpr uint16_t kPort = 55016;
    auto deviceBuffers = makeRingBuffers(kTestRingSize);
    RTPReceiver receiver(makeRxSDP(kGroup, kPort, 2), makeMapping(2, 0), deviceBuffers);
    CHECK(receiver.start(), "receiver should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ChannelDrainer drainer(deviceBuffers[0]);
    LoopbackSender sender(kGroup, kPort);
    constexpr uint16_t kPackets = 60;
    auto next = std::chrono::steady_clock::now();
    for (uint16_t i = 0; i < kPackets; ++i) {
        sender.sendL24(static_cast<uint16_t>(3000 + i), i * kFramesPerPacket, 2, 0.25f);
        if (i == 30) {
            sender.sendL24(static_cast<uint16_t>(3000 - 400), 0, 2, 0.9f);  // 400 packets late
        }
        std::this_thread::sleep_until(next += std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    receiver.stop();
    const auto samples = drainer.stop();

    CHECK(countNear(samples, 0.9f) == 0, "the late packet should not be played");
    CHECK(countNear(samples, 0.25f) == kPackets * kFramesPerPacket,
          "every in-order packet should still be played (got " << countNear(samples, 0.25f) / kFramesPerPacket
                                                               << " of " << kPackets << ")");
    CHECK(countNear(samples, 0.0f) == 0, "a late packet should not cause silence");
}

// ---------------------------------------------------------------------------
// A second sender on the same group and port (different SSRC and sequence
// base) must not take over or disrupt the stream being played.
// ---------------------------------------------------------------------------
void testInterleavedSecondSourceIsIgnored() {
    std::cout << "RX ignores an interleaved second source" << std::endl;

    constexpr const char* kGroup = "239.69.99.6";
    constexpr uint16_t kPort = 55018;
    auto deviceBuffers = makeRingBuffers(kTestRingSize);
    RTPReceiver receiver(makeRxSDP(kGroup, kPort, 2), makeMapping(2, 0), deviceBuffers);
    CHECK(receiver.start(), "receiver should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ChannelDrainer drainer(deviceBuffers[0]);
    LoopbackSender sender(kGroup, kPort);
    constexpr uint16_t kPackets = 100;
    auto next = std::chrono::steady_clock::now();
    for (uint16_t i = 0; i < kPackets; ++i) {
        sender.sendL24(static_cast<uint16_t>(5000 + i), i * kFramesPerPacket, 2, 0.25f, 0xAAAA0001);
        sender.sendL24(static_cast<uint16_t>(20000 + i), 777 + i * kFramesPerPacket, 2, 0.9f, 0xBBBB0002);
        std::this_thread::sleep_until(next += std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    receiver.stop();
    const auto samples = drainer.stop();

    CHECK(countNear(samples, 0.9f) == 0, "the second source should never be played");
    CHECK(countNear(samples, 0.25f) == kPackets * kFramesPerPacket,
          "the first source should play uninterrupted (got " << countNear(samples, 0.25f) / kFramesPerPacket
                                                             << " of " << kPackets << " packets)");
}

// ---------------------------------------------------------------------------
// A sender restart whose new sequence base lands just BEHIND the playout point
// (within the jitter window) must still be followed. Packets behind the
// consumer can never be played, so without a resync the stream stays silent.
// ---------------------------------------------------------------------------
void runRestartBehindPlayout(const char* group, uint16_t port, uint32_t restartSsrc, const char* label) {
    auto deviceBuffers = makeRingBuffers(kTestRingSize);
    RTPReceiver receiver(makeRxSDP(group, port, 2), makeMapping(2, 0), deviceBuffers);
    CHECK(receiver.start(), label << ": receiver should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ChannelDrainer drainer(deviceBuffers[0]);
    LoopbackSender sender(group, port);
    auto next = std::chrono::steady_clock::now();
    for (uint16_t i = 0; i < 60; ++i) {
        sender.sendL24(static_cast<uint16_t>(6000 + i), i * kFramesPerPacket, 2, 0.25f);
        std::this_thread::sleep_until(next += std::chrono::milliseconds(1));
    }
    // Restart about 150 packets behind where playout has reached
    for (uint16_t i = 0; i < 150; ++i) {
        sender.sendL24(static_cast<uint16_t>(5900 + i), 500000 + i * kFramesPerPacket, 2, -0.5f, restartSsrc);
        std::this_thread::sleep_until(next += std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    receiver.stop();
    const auto samples = drainer.stop();

    CHECK(countNear(samples, -0.5f) >= 100 * kFramesPerPacket,
          label << ": the restarted stream should play (got " << countNear(samples, -0.5f) / kFramesPerPacket
                << " of 150 packets)");
}

void testRestartBehindPlayoutIsFollowed() {
    std::cout << "RX follows a restart that lands behind playout" << std::endl;
    runRestartBehindPlayout("239.69.99.7", 55020, 0x12345678, "same SSRC");
    runRestartBehindPlayout("239.69.99.8", 55022, 0x0BADF00D, "new SSRC");
}

// ---------------------------------------------------------------------------
// After an outage the consumer has already underrun for most of the gap, and
// Core Audio has played that time as zeros. The missing packets must not then
// be played as silence again, or every outage adds its length to the latency.
// ---------------------------------------------------------------------------
void testOutageDoesNotAddLatency() {
    std::cout << "RX outage does not add latency" << std::endl;

    constexpr const char* kGroup = "239.69.99.9";
    constexpr uint16_t kPort = 55024;
    auto deviceBuffers = makeRingBuffers(kTestRingSize);
    RTPReceiver receiver(makeRxSDP(kGroup, kPort, 2), makeMapping(2, 0), deviceBuffers);
    CHECK(receiver.start(), "receiver should start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ChannelDrainer drainer(deviceBuffers[0]);
    LoopbackSender sender(kGroup, kPort);
    constexpr uint16_t kOutagePackets = 40;
    auto next = std::chrono::steady_clock::now();
    for (uint16_t i = 0; i < 100; ++i) {
        // Packets 30..69 are lost in a 40 ms outage
        if (i < 30 || i >= 30 + kOutagePackets) {
            sender.sendL24(static_cast<uint16_t>(7000 + i), i * kFramesPerPacket, 2, 0.25f);
        }
        std::this_thread::sleep_until(next += std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    receiver.stop();
    const auto samples = drainer.stop();

    const size_t silencePackets = countNear(samples, 0.0f) / kFramesPerPacket;
    CHECK(countNear(samples, 0.25f) == (100 - kOutagePackets) * kFramesPerPacket, "every received packet should play");
    // Idle runs give ~5 (the prefill cushion being rebuilt). Under heavy CPU
    // load the sender thread itself stalls and bursts, shortening the outage in
    // wall time, so fewer losses are covered by underruns; the regression this
    // guards against silences every lost packet (exactly kOutagePackets).
    CHECK(silencePackets < kOutagePackets / 2,
          "only the cushion should be re-filled with silence, not the whole outage (got "
              << silencePackets << " packets of silence for a " << kOutagePackets << "-packet outage)");
}

} // namespace

int main() {
    testMixedOutputReachesOutputBuffers();
    testLargeIOBuffersAreChunked();
    testDeviceSampleRate();
    testRateControllerTracksSender();
    testTxStreamSurvivesReload();
    testJitterBufferReplacesStaleSlot();
    testReceiverResyncsAfterSenderRestart();
    testLostPacketBecomesSilence();
    testLatePacketDoesNotResync();
    testInterleavedSecondSourceIsIgnored();
    testRestartBehindPlayoutIsFollowed();
    testOutageDoesNotAddLatency();

    std::cout << "\nCritical path regressions: " << checksPassed << " passed, "
              << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
