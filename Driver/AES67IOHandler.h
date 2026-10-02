//
// AES67IOHandler.h
// AES67 macOS Driver - Build #1
// Real-time safe audio I/O handler for Core Audio
// NO ALLOCATION, NO LOCKS, NO BLOCKING
//

#pragma once

#include "../Shared/Types.h"
#include "../Shared/RingBuffer.hpp"
#include "../NetworkEngine/RTSafeStreamInterface.h"
#include <aspl/IORequestHandler.hpp>
#include <aspl/Client.hpp>
#include <aspl/Stream.hpp>
#include <memory>
#include <array>
#include <atomic>

class IOHandlerBenchmark;

namespace AES67 {

//
// AES67 IO Handler
//
// Handles real-time audio I/O between Core Audio and ring buffers
// Called from Core Audio's real-time thread - MUST be RT-safe!
//
// RT-SAFE REQUIREMENTS:
// - NO memory allocation (malloc/new/delete)
// - NO locks (mutexes, semaphores)
// - NO blocking operations
// - NO system calls that can block
// - NO Objective-C message sends
// - Bounded execution time
//
class AES67IOHandler : public aspl::IORequestHandler {
public:
    using DeviceChannelBuffers = std::array<SPSCRingBuffer<float>, 128>;

    //
    // Constructor
    //
    // Takes an RTSafeStreamInterface which provides the RT-safe boundary:
    // only lock-free ring buffers and atomic counters, no StreamManager access.
    //
    AES67IOHandler(
        RTSafeStreamInterface& rtInterface,
        UInt32 channelCount = 128,
        UInt32 bytesPerSample = sizeof(Float32)
    );

    ~AES67IOHandler() override;

    //
    // aspl::IORequestHandler overrides (RT-SAFE!)
    //

    // Called when Core Audio needs input data from device
    // Reads from inputBuffers_ and provides raw bytes to Core Audio
    void OnReadClientInput(
        const std::shared_ptr<aspl::Client>& client,
        const std::shared_ptr<aspl::Stream>& stream,
        Float64 zeroTimestamp,
        Float64 timestamp,
        void* bytes,
        UInt32 bytesCount
    ) override;

    // Called when Core Audio has output data for device
    // Receives the mix of all clients (DeviceParameters::EnableMixing is true,
    // so libASPL never calls OnWriteClientOutput) and writes it to outputBuffers_
    void OnWriteMixedOutput(
        const std::shared_ptr<aspl::Stream>& stream,
        Float64 zeroTimestamp,
        Float64 timestamp,
        const void* bytes,
        UInt32 bytesCount
    ) override;

private:
    // Process input stream (Network → Core Audio)
    // RT-SAFE: Reads from ring buffers, fills silence on underrun
    // Uses batch processing for optimal performance
    void processInput(float* outputData, UInt32 frameCount, UInt32 channelCount) noexcept;

    // Process output stream (Core Audio → Network)
    // RT-SAFE: Writes to ring buffers, discards on overrun
    // Uses batch processing for optimal performance
    void processOutput(const float* inputData, UInt32 frameCount, UInt32 channelCount) noexcept;

    // RT-safe interface (compile-time boundary)
    // Provides lock-free access to ring buffers and atomic counters.
    // This is the ONLY path through which the IO handler accesses audio data.
    // It does NOT provide access to StreamManager or any locked resources.
    RTSafeStreamInterface& rtInterface_;

    // Cached audio format (set at construction, avoids virtual calls in RT path)
    const UInt32 cachedChannelCount_;
    const UInt32 cachedBytesPerSample_;
    const UInt32 cachedBytesPerFrame_;   // cachedChannelCount_ * cachedBytesPerSample_

    // Constants
    static constexpr size_t kNumChannels = 128;

    // Largest block processInput/processOutput handle at once (stack scratch size)
    static constexpr UInt32 kMaxFramesPerChunk = 4096;

    // Allow benchmark direct access to processInput/processOutput
    friend class ::IOHandlerBenchmark;
};

} // namespace AES67
