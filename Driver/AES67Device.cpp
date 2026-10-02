//
// AES67Device.cpp
// AES67 macOS Driver - Build #9
// Core Audio device implementation
//

#include "AES67Device.h"
#include "AES67IOHandler.h"
#include "SDPParser.h"
#include "DebugLog.h"
#include "../NetworkEngine/Clock/DeviceTimeline.h"
#include "../NetworkEngine/Clock/HostTime.h"
#include <CoreAudio/AudioServerPlugIn.h>
#include <utility>

namespace AES67 {

// Helper to create initialized ring buffer array
namespace {
    template<size_t... Is>
    auto MakeRingBufferArray(size_t bufferSize, std::index_sequence<Is...>) {
        return std::array<SPSCRingBuffer<float>, sizeof...(Is)>{
            ((void)Is, SPSCRingBuffer<float>(bufferSize))...
        };
    }

    auto MakeRingBufferArray(size_t bufferSize) {
        return MakeRingBufferArray(bufferSize, std::make_index_sequence<AES67Device::kNumChannels>{});
    }
}

AES67Device::AES67Device(std::shared_ptr<aspl::Context> context)
    : aspl::Device(context, aspl::DeviceParameters{
        .Name = "AES67 Device",
        .Manufacturer = "AES67 Driver",
        .DeviceUID = "com.aes67.driver.device",
        .ModelUID = "com.aes67.driver.model",
        .CanBeDefault = true,
        .CanBeDefaultForSystemSounds = false,
        .SampleRate = kDefaultSampleRate,
        .ZeroTimeStampPeriod = kZeroTimeStampPeriod,
        .ClockIsStable = true,
        .ClockAlgorithm = kAudioDeviceClockAlgorithmRaw
    })
    // Initialize ring buffers sized for maximum supported sample rate (384kHz)
    // This ensures buffers are always large enough regardless of sample rate changes
    // Power-of-2 sizing: 384kHz @ 3ms = 1152 samples → 2048 (next power of 2)
    , inputBuffers_(MakeRingBufferArray(
          CalculateRingBufferSize(384000.0)))  // Max sample rate
    , outputBuffers_(MakeRingBufferArray(
          CalculateRingBufferSize(384000.0)))  // Max sample rate
{
    AES67_LOG("AES67Device constructor: Starting initialization");
    const Float64 initialSampleRate = currentSampleRate_.load();
    const size_t ringBufferSize = CalculateRingBufferSize(384000.0);
    AES67_LOGF("AES67Device: Initial sample rate = %.0f Hz", initialSampleRate);
    AES67_LOGF("AES67Device: Ring buffer size = %zu samples (sized for max 384kHz)",
               ringBufferSize);
    AES67_LOGF("AES67Device: Buffer latency @ %.0f Hz = %.2f ms",
               initialSampleRate,
               (ringBufferSize * 1000.0) / initialSampleRate);

    // NOTE: Cannot call InitializeStreams() here because shared_from_this()
    // won't work until the shared_ptr is fully constructed
    // InitializeStreams() will be called from Initialize() method

    AES67_LOG("AES67Device constructor: Basic initialization complete");
}

void AES67Device::Initialize() {
    AES67_LOG("AES67Device::Initialize() called");

    // Valid clock from the start; restarted again whenever IO starts
    RestartTimeline(currentSampleRate_.load());

    // Initialize streams
    AES67_LOG("AES67Device: Calling InitializeStreams()");
    InitializeStreams();

    // Create RT-safe interface (compile-time boundary for IO handler)
    // Must be created before InitializeIOHandler() so it can be passed in.
    AES67_LOG("AES67Device: Creating RTSafeStreamInterface");
    rtInterface_ = std::make_unique<RTSafeStreamInterface>(
        inputBuffers_,
        outputBuffers_,
        inputUnderruns_,
        outputUnderruns_,
        ioRunning_
    );
    AES67_LOG("AES67Device: RTSafeStreamInterface created successfully");

    // Initialize IO handler (uses RT-safe interface)
    AES67_LOG("AES67Device: Calling InitializeIOHandler()");
    InitializeIOHandler();

    // Initialize Stream Manager (manages all AES67 network streams)
    AES67_LOG("AES67Device: Creating StreamManager");
    streamManager_ = std::make_unique<StreamManager>(inputBuffers_, outputBuffers_);
    AES67_LOG("AES67Device: StreamManager created successfully");

    // Set device sample rate in StreamManager
    streamManager_->setDeviceSampleRate(currentSampleRate_.load());
    AES67_LOGF("AES67Device: StreamManager sample rate set to %.0f Hz", currentSampleRate_.load());

    // Load saved stream configurations from disk
    AES67_LOG("AES67Device: Attempting to load saved stream configurations");
    bool loadedSavedStreams = streamManager_->loadSavedStreams();

    // If no saved streams were loaded, create test streams for initial testing
    if (!loadedSavedStreams) {
        // Create test RX stream (Network → Core Audio) on channels 0-7
        AES67_LOG("AES67Device: No saved streams found, adding test RX stream (239.1.1.1:5004, 8ch @ 48kHz)");
        SDPSession testSDP;
        testSDP.sessionName = "Test AES67 Stream";
        testSDP.sessionInfo = "Hard-coded test stream for driver development";
        testSDP.connectionAddress = "239.1.1.1";  // AES67 multicast range
        testSDP.port = 5004;
        testSDP.numChannels = 8;
        testSDP.sampleRate = 48000.0;
        testSDP.encoding = "L24";
        testSDP.payloadType = 97;
        testSDP.ptime = 1.0;  // 1ms packets (48 samples @ 48kHz)
        testSDP.framecount = 48;
        testSDP.ptpDomain = 0;
        testSDP.sessionID = 123456;
        testSDP.sessionVersion = 1;
        testSDP.sourceAddress = "0.0.0.0";

        StreamID testStreamID = streamManager_->addStream(testSDP);
        if (!testStreamID.isNull()) {
            AES67_LOG("AES67Device: Test RX stream added successfully");
            AES67_LOGF("AES67Device: Test RX stream ID: %s", testStreamID.toString().c_str());
        } else {
            AES67_LOG("AES67Device: WARNING - Failed to add test RX stream");
        }

        // Create test TX stream (Core Audio → Network) on channels 8-15
        // Uses a different multicast group (239.1.1.2) to avoid confusion with RX
        AES67_LOG("AES67Device: Adding test TX stream (239.1.1.2:5004, 8ch @ 48kHz, channels 8-15)");
        ChannelMapping txMapping;
        txMapping.streamChannelCount = 8;
        txMapping.deviceChannelStart = 8;   // Channels 8-15 (non-overlapping with RX 0-7)
        txMapping.deviceChannelCount = 8;

        StreamID txStreamID = streamManager_->createTxStream(
            "Test AES67 TX Stream",
            "239.1.1.2",   // Different multicast group from RX (239.1.1.1)
            5004,
            8,
            txMapping
        );
        if (!txStreamID.isNull()) {
            AES67_LOG("AES67Device: Test TX stream added successfully");
            AES67_LOGF("AES67Device: Test TX stream ID: %s", txStreamID.toString().c_str());
        } else {
            AES67_LOG("AES67Device: WARNING - Failed to add test TX stream");
        }
    }

    AES67_LOG("AES67Device::Initialize() complete");
}

AES67Device::~AES67Device() {
    // Deactivate streams directly rather than calling StopIO() (which requires
    // framework context). This is safe in the destructor.
    if (inputStream_) {
        inputStream_->SetIsActive(false);
    }
    if (outputStream_) {
        outputStream_->SetIsActive(false);
    }
    ioRunning_.store(false);
}

void AES67Device::InitializeStreams() {
    AES67_LOG("InitializeStreams: Creating input stream (Network → Core Audio)");
    // Create input stream (Network → Core Audio)
    aspl::StreamParameters inputParams;
    inputParams.Direction = aspl::Direction::Input;
    inputParams.StartingChannel = 1;
    inputParams.Format.mSampleRate = currentSampleRate_.load();
    inputParams.Format.mFormatID = kAudioFormatLinearPCM;
    inputParams.Format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    inputParams.Format.mBitsPerChannel = 32;
    inputParams.Format.mChannelsPerFrame = kNumChannels;
    inputParams.Format.mBytesPerFrame = kNumChannels * sizeof(float);
    inputParams.Format.mFramesPerPacket = 1;
    inputParams.Format.mBytesPerPacket = inputParams.Format.mBytesPerFrame;

    AES67_LOGF("InitializeStreams: Input stream - %u channels @ %.0f Hz",
               kNumChannels, currentSampleRate_.load());

    inputStream_ = std::make_shared<aspl::Stream>(
        GetContext(),
        std::static_pointer_cast<aspl::Device>(shared_from_this()),
        inputParams
    );
    AES67_LOG("InitializeStreams: Input stream created, adding to device");
    AddStreamAsync(inputStream_);
    AES67_LOG("InitializeStreams: Input stream added successfully");

    // Create output stream (Core Audio → Network)
    AES67_LOG("InitializeStreams: Creating output stream (Core Audio → Network)");
    aspl::StreamParameters outputParams;
    outputParams.Direction = aspl::Direction::Output;
    outputParams.StartingChannel = 1;
    outputParams.Format.mSampleRate = currentSampleRate_.load();
    outputParams.Format.mFormatID = kAudioFormatLinearPCM;
    outputParams.Format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    outputParams.Format.mBitsPerChannel = 32;
    outputParams.Format.mChannelsPerFrame = kNumChannels;
    outputParams.Format.mBytesPerFrame = kNumChannels * sizeof(float);
    outputParams.Format.mFramesPerPacket = 1;
    outputParams.Format.mBytesPerPacket = outputParams.Format.mBytesPerFrame;

    AES67_LOGF("InitializeStreams: Output stream - %u channels @ %.0f Hz",
               kNumChannels, currentSampleRate_.load());

    outputStream_ = std::make_shared<aspl::Stream>(
        GetContext(),
        std::static_pointer_cast<aspl::Device>(shared_from_this()),
        outputParams
    );
    AES67_LOG("InitializeStreams: Output stream created, adding to device");
    AddStreamAsync(outputStream_);
    AES67_LOG("InitializeStreams: Output stream added successfully");

    AES67_LOG("InitializeStreams: Complete");
}

void AES67Device::InitializeIOHandler() {
    AES67_LOG("InitializeIOHandler: Creating AES67IOHandler with RTSafeStreamInterface");
    ioHandler_ = std::make_shared<AES67IOHandler>(
        *rtInterface_,
        kNumChannels,           // Cache channel count for RT-safe access
        sizeof(Float32)         // Cache bytes per sample for RT-safe access
    );
    AES67_LOG("InitializeIOHandler: IOHandler created successfully");

    // Register IO handler with device
    AES67_LOG("InitializeIOHandler: Registering IOHandler with device");
    SetIOHandler(ioHandler_);
    AES67_LOG("InitializeIOHandler: Complete");
}

Float64 AES67Device::GetSampleRate() const {
    return currentSampleRate_.load();
}

bool AES67Device::IsSupportedSampleRate(Float64 sampleRate) {
    for (auto validRate : kSupportedSampleRates) {
        if (std::abs(sampleRate - validRate) < 0.1) {
            return true;
        }
    }
    return false;
}

OSStatus AES67Device::SetNominalSampleRateImpl(Float64 rate) {
    if (!IsSupportedSampleRate(rate)) {
        return kAudioHardwareUnsupportedOperationError;
    }

    const Float64 previousRate = currentSampleRate_.load();
    AES67_LOGF("SetNominalSampleRateImpl: Changing from %.0f Hz to %.0f Hz", previousRate, rate);

    // StreamManager refuses rates its active streams cannot deliver (no SRC yet)
    if (streamManager_ && !streamManager_->setDeviceSampleRate(rate)) {
        AES67_LOGF("SetNominalSampleRateImpl: Refused %.0f Hz - active streams run at a different rate", rate);
        return kAudioDeviceUnsupportedFormatError;
    }

    const OSStatus status = aspl::Device::SetNominalSampleRateImpl(rate);
    if (status != kAudioHardwareNoError) {
        if (streamManager_) {
            streamManager_->setDeviceSampleRate(previousRate);
        }
        return status;
    }

    currentSampleRate_.store(rate);

    // Media positions are counted in samples, so a new rate is a new timeline
    RestartTimeline(rate);

    // Streams must present the same rate as the device. We are inside a HAL
    // configuration change, so these Async setters apply in place.
    if (inputStream_) {
        ApplyStreamSampleRate(*inputStream_, rate);
    }
    if (outputStream_) {
        ApplyStreamSampleRate(*outputStream_, rate);
    }

    AES67_LOGF("SetNominalSampleRateImpl: Now running at %.0f Hz (ring buffer %.2f ms)",
               rate, (inputBuffers_[0].capacity() * 1000.0) / rate);
    return kAudioHardwareNoError;
}

void AES67Device::RestartTimeline(Float64 sampleRate) {
    std::lock_guard<std::mutex> lock(clockWriteMutex_);
    mediaClock_.reset(hostTimeNow(), 0,
                      MediaClock::samplesPerTick(sampleRate, 1.0, HostTimebase::current()));
}

OSStatus AES67Device::GetZeroTimeStampImpl(UInt32 clientID, Float64* outSampleTime, UInt64* outHostTime,
                                           UInt64* outSeed) {
    // RT-SAFE: lock-free snapshot, no allocation
    const ZeroTimeStamp zts = zeroTimeStampAt(mediaClock_.snapshot(), hostTimeNow(), kZeroTimeStampPeriod);
    *outSampleTime = zts.sampleTime;
    *outHostTime = zts.hostTime;
    *outSeed = zts.seed;
    (void)clientID;
    return kAudioHardwareNoError;
}

void AES67Device::ApplyStreamSampleRate(aspl::Stream& stream, Float64 sampleRate) {
    auto format = stream.GetPhysicalFormat();
    format.mSampleRate = sampleRate;

    const AudioStreamRangedDescription ranged{format, {sampleRate, sampleRate}};
    stream.SetAvailablePhysicalFormatsAsync({ranged});
    stream.SetAvailableVirtualFormatsAsync({ranged});
    stream.SetPhysicalFormatAsync(format);
    stream.SetVirtualFormatAsync(format);
}

std::vector<AudioValueRange> AES67Device::GetAvailableSampleRates() const {
    std::vector<AudioValueRange> ranges;
    for (auto rate : kSupportedSampleRates) {
        if (!streamManager_ || streamManager_->isSampleRateCompatible(rate)) {
            ranges.push_back({rate, rate});
        }
    }
    return ranges;
}

UInt32 AES67Device::GetBufferSize() const {
    return currentBufferSize_.load();
}

OSStatus AES67Device::SetBufferSize(UInt32 bufferSize) {
    // Validate buffer size
    bool isValid = false;
    for (auto validSize : kSupportedBufferSizes) {
        if (bufferSize == validSize) {
            isValid = true;
            break;
        }
    }

    if (!isValid) {
        return kAudioHardwareUnsupportedOperationError;
    }

    // Update current buffer size
    currentBufferSize_.store(bufferSize);

    return kAudioHardwareNoError;
}

std::vector<UInt32> AES67Device::GetAvailableBufferSizes() const {
    return std::vector<UInt32>(kSupportedBufferSizes.begin(), kSupportedBufferSizes.end());
}

std::string AES67Device::GetDeviceName() const {
    return "AES67 Device";
}

std::string AES67Device::GetDeviceManufacturer() const {
    return "AES67 Driver";
}

std::string AES67Device::GetDeviceUID() const {
    return "com.aes67.driver.device";
}

OSStatus AES67Device::StartIOImpl(UInt32 clientID, UInt32 startCount) {
    // startCount == 0 means first client starting IO (device transitions to running)
    if (startCount == 0) {
        // Activate streams
        if (inputStream_) {
            inputStream_->SetIsActive(true);
        }
        if (outputStream_) {
            outputStream_->SetIsActive(true);
        }

        ioRunning_.store(true);

        // New timeline for this IO session (new seed for the HAL)
        RestartTimeline(currentSampleRate_.load());

        // Start RTP network threads now that a client needs audio
        if (streamManager_) {
            streamManager_->setIOActive(true);
        }
    }

    return aspl::Device::StartIOImpl(clientID, startCount);
}

OSStatus AES67Device::StopIOImpl(UInt32 clientID, UInt32 startCount) {
    // startCount == 0 means last client stopped IO (device transitions to not running)
    if (startCount == 0) {
        // Deactivate streams
        if (inputStream_) {
            inputStream_->SetIsActive(false);
        }
        if (outputStream_) {
            outputStream_->SetIsActive(false);
        }

        // Stop RTP network threads — no client needs audio anymore
        if (streamManager_) {
            streamManager_->setIOActive(false);
        }

        ioRunning_.store(false);
    }

    return aspl::Device::StopIOImpl(clientID, startCount);
}

void AES67Device::ResetStatistics() {
    inputUnderruns_.store(0);
    outputUnderruns_.store(0);
}

OSStatus AES67Device::OnSetBufferSize(UInt32 bufferSize) {
    return SetBufferSize(bufferSize);
}

size_t AES67Device::CalculateRingBufferSize(Float64 sampleRate, double latencyMs) {
    // Calculate ring buffer size for desired latency
    // Formula: samples = (sampleRate × latencyMs) / 1000
    //
    // Examples (with 3ms latency):
    //   48kHz @ 3ms = 144 samples → 256 (rounded to power of 2)
    //   96kHz @ 3ms = 288 samples → 512
    //   192kHz @ 3ms = 576 samples → 1024
    //   384kHz @ 3ms = 1152 samples → 2048
    //
    // Minimum 3ms buffer provides adequate tolerance for:
    // - Network jitter (typical: 0.5-1ms)
    // - Processing delays (typical: 0.5-1ms)
    // - Scheduling variations (typical: 0.5-1ms)
    //
    // Power-of-2 sizing enables efficient modulo operations

    // Calculate minimum size based on latency requirement
    const size_t minSize = static_cast<size_t>(
        (sampleRate * latencyMs) / 1000.0
    );

    // Round up to next power of 2 for efficient modulo operations
    size_t size = 1;
    while (size < minSize) {
        size <<= 1;
    }

    // Enforce absolute minimum (512 samples = 10.6ms @ 48kHz, 1.3ms @ 384kHz)
    constexpr size_t kMinRingBufferSize = 512;

    // Enforce maximum to prevent excessive memory (8192 samples = 21.3ms @ 384kHz)
    // At 128 channels × 4 bytes/sample: 8192 × 128 × 4 = 4MB per buffer direction
    constexpr size_t kMaxRingBufferSize = 8192;

    size = std::max(size, kMinRingBufferSize);
    size = std::min(size, kMaxRingBufferSize);

    return size;
}

} // namespace AES67
