//
// AES67Device.h
// AES67 macOS Driver - Build #1
// Core Audio device implementation using libASPL
// 128-channel input/output AudioServerPlugIn
//

#pragma once

#include "../Shared/Types.h"
#include "../Shared/RingBuffer.hpp"
#include "../NetworkEngine/StreamManager.h"
#include "../NetworkEngine/RTSafeStreamInterface.h"
#include <aspl/Device.hpp>
#include <aspl/Stream.hpp>
#include <aspl/Context.hpp>
#include <memory>
#include <array>
#include <atomic>

namespace AES67 {

class AES67IOHandler;

//
// AES67 Audio Device
//
// 128-channel bidirectional Core Audio device
// Integrates with macOS Core Audio via AudioServerPlugIn (libASPL)
//
class AES67Device : public aspl::Device {
public:
    static constexpr size_t kNumChannels = 128;

    // Nominal rate at startup (AES67 baseline). Must be passed to libASPL via
    // DeviceParameters::SampleRate, whose own default is 44.1 kHz.
    static constexpr UInt32 kDefaultSampleRate = 48000;

    // Supported sample rates
    static constexpr std::array<Float64, 8> kSupportedSampleRates = {
        44100.0, 48000.0, 88200.0, 96000.0,
        176400.0, 192000.0, 352800.0, 384000.0
    };

    // Supported buffer sizes (in samples)
    static constexpr std::array<UInt32, 8> kSupportedBufferSizes = {
        16, 32, 48, 64, 128, 192, 288, 480
    };

    //
    // Constructor
    //
    explicit AES67Device(std::shared_ptr<aspl::Context> context);
    ~AES67Device();

    // Initialize device (must be called after construction)
    void Initialize();

    //
    // Device Configuration
    //

    // Current sample rate as seen by the driver (follows the HAL nominal rate)
    Float64 GetSampleRate() const;

    // Rates offered to the HAL: supported rates that every active stream can deliver
    std::vector<AudioValueRange> GetAvailableSampleRates() const override;

    // Get/Set buffer size
    UInt32 GetBufferSize() const;
    OSStatus SetBufferSize(UInt32 bufferSize);

    // Get available buffer sizes
    std::vector<UInt32> GetAvailableBufferSizes() const;

    //
    // Device Information
    //

    std::string GetDeviceName() const;
    std::string GetDeviceManufacturer() const;
    std::string GetDeviceUID() const override;

    // Get channel count
    UInt32 GetInputChannelCount() const { return kNumChannels; }
    UInt32 GetOutputChannelCount() const { return kNumChannels; }

    //
    // Stream Access
    //

    // Get input/output streams
    std::shared_ptr<aspl::Stream> GetInputStream() const { return inputStream_; }
    std::shared_ptr<aspl::Stream> GetOutputStream() const { return outputStream_; }

    //
    // Ring Buffer Access (for NetworkEngine)
    //

    using DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, kNumChannels>;

    DeviceChannelBuffers& GetInputBuffers() { return inputBuffers_; }
    DeviceChannelBuffers& GetOutputBuffers() { return outputBuffers_; }

    //
    // RT-Safe Interface Access
    //

    // Returns the RT-safe interface for use by AES67IOHandler.
    // The interface is created during Initialize() and is valid for the
    // lifetime of this device. Only pass this to RT-safe code paths.
    RTSafeStreamInterface* GetRTInterface() { return rtInterface_.get(); }
    const RTSafeStreamInterface* GetRTInterface() const { return rtInterface_.get(); }

    //
    // Stream Manager Access
    //

    StreamManager* GetStreamManager() { return streamManager_.get(); }
    const StreamManager* GetStreamManager() const { return streamManager_.get(); }

    //
    // Control
    //

    // Start/Stop IO (overrides from aspl::Device)
    OSStatus StartIOImpl(UInt32 clientID, UInt32 startCount) override;
    OSStatus StopIOImpl(UInt32 clientID, UInt32 startCount) override;

    //
    // Statistics
    //

    uint64_t GetInputUnderrunCount() const { return inputUnderruns_.load(); }
    uint64_t GetOutputUnderrunCount() const { return outputUnderruns_.load(); }
    void ResetStatistics();

protected:
    // Invoked by libASPL (inside a HAL configuration change) when a client sets
    // kAudioDevicePropertyNominalSampleRate. Applies the rate to the driver,
    // StreamManager and both streams' formats.
    OSStatus SetNominalSampleRateImpl(Float64 rate) override;

private:
    // Internal handlers
    OSStatus OnSetBufferSize(UInt32 bufferSize);

    static bool IsSupportedSampleRate(Float64 sampleRate);

    // Lock a stream's physical and virtual formats to the given sample rate
    static void ApplyStreamSampleRate(aspl::Stream& stream, Float64 sampleRate);

    // Initialize streams and IO handler
    void InitializeStreams();
    void InitializeIOHandler();

    // Calculate optimal ring buffer size based on sample rate
    // Returns size for desired latency (default: 3ms for network jitter tolerance)
    // Result is rounded up to power of 2 for efficient modulo operations
    static size_t CalculateRingBufferSize(Float64 sampleRate, double latencyMs = 3.0);

    // Ring buffers for audio data
    // Network threads write to input buffers, read from output buffers
    // Core Audio thread reads from input buffers, writes to output buffers
    DeviceChannelBuffers inputBuffers_;   // Network → CoreAudio
    DeviceChannelBuffers outputBuffers_;  // CoreAudio → Network

    // Streams
    std::shared_ptr<aspl::Stream> inputStream_;
    std::shared_ptr<aspl::Stream> outputStream_;

    // IO Handler
    std::shared_ptr<AES67IOHandler> ioHandler_;

    // Stream Manager (manages all AES67 network streams)
    std::unique_ptr<StreamManager> streamManager_;

    // RT-safe interface (compile-time boundary for IO handler)
    // Created during Initialize(), references inputBuffers_/outputBuffers_/atomics
    std::unique_ptr<RTSafeStreamInterface> rtInterface_;

    // Current configuration
    std::atomic<Float64> currentSampleRate_{static_cast<Float64>(kDefaultSampleRate)};
    std::atomic<UInt32> currentBufferSize_{64};

    // State
    std::atomic<bool> ioRunning_{false};

    // Statistics
    std::atomic<uint64_t> inputUnderruns_{0};
    std::atomic<uint64_t> outputUnderruns_{0};
};

} // namespace AES67
