//
// AES67IOHandler.cpp
// AES67 macOS Driver - Build #6
// Real-time safe audio I/O handler
// RT-SAFE: NO ALLOCATION, NO LOCKS, NO BLOCKING
//

#include "AES67IOHandler.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace AES67 {

AES67IOHandler::AES67IOHandler(
    RTSafeStreamInterface& rtInterface,
    UInt32 channelCount,
    UInt32 bytesPerSample
)
    : rtInterface_(rtInterface)
    , cachedChannelCount_(channelCount)
    , cachedBytesPerSample_(bytesPerSample)
    , cachedBytesPerFrame_(channelCount * bytesPerSample)
{
}

AES67IOHandler::~AES67IOHandler() {
}

void AES67IOHandler::OnReadClientInput(
    const std::shared_ptr<aspl::Client>& client,
    const std::shared_ptr<aspl::Stream>& stream,
    Float64 zeroTimestamp,
    Float64 timestamp,
    void* bytes,
    UInt32 bytesCount
) {
    // RT-SAFE: Read receive buffers by timestamp (Network → Core Audio)
    // This provides INPUT audio to the client (DAW). Reads are non-destructive,
    // so every client reading the same cycle hears the same audio.
    //
    // libASPL calls this with raw bytes in the stream's native format.
    // Our stream format is 32-bit float, so bytesCount = frameCount * channelCount * 4.

    if (!bytes) {
        return;
    }

    // RT-SAFE: Use cached format values instead of calling stream->GetPhysicalFormat()
    // (virtual method call is not safe on the RT audio thread)
    const UInt32 channelCount = cachedChannelCount_;
    const UInt32 bytesPerFrame = cachedBytesPerFrame_;
    const UInt32 frameCount = (bytesPerFrame > 0) ? (bytesCount / bytesPerFrame) : 0;

    if (frameCount == 0 || channelCount != kNumChannels) {
        std::memset(bytes, 0, bytesCount);
        return;
    }

    processInput(static_cast<float*>(bytes), frameCount, channelCount, timestamp);

    (void)client;
    (void)stream;
    (void)zeroTimestamp;
}

void AES67IOHandler::OnWriteMixedOutput(
    const std::shared_ptr<aspl::Stream>& stream,
    Float64 zeroTimestamp,
    Float64 timestamp,
    const void* bytes,
    UInt32 bytesCount
) {
    // RT-SAFE: Write to ring buffers (Core Audio → Network)
    // This receives the mixed OUTPUT audio of all clients (DAWs, system audio)
    //
    // libASPL provides raw bytes in the stream's native format, which is
    // interleaved 32-bit float, so bytesCount = frameCount * channelCount * 4.

    if (!bytes) {
        return;
    }

    // RT-SAFE: Use cached format values instead of calling stream->GetPhysicalFormat()
    const UInt32 channelCount = cachedChannelCount_;
    const UInt32 bytesPerFrame = cachedBytesPerFrame_;
    const UInt32 frameCount = (bytesPerFrame > 0) ? (bytesCount / bytesPerFrame) : 0;

    if (frameCount == 0 || channelCount != kNumChannels) {
        return;
    }

    // Process in chunks that fit processOutput's stack scratch buffer
    const float* input = static_cast<const float*>(bytes);
    for (UInt32 done = 0; done < frameCount;) {
        const UInt32 chunk = std::min(frameCount - done, kMaxFramesPerChunk);
        processOutput(input + static_cast<size_t>(done) * channelCount, chunk, channelCount);
        done += chunk;
    }

    (void)stream;
    (void)zeroTimestamp;
    (void)timestamp;
}

void AES67IOHandler::processInput(float* outputData, UInt32 frameCount, UInt32 channelCount,
                                  Float64 sampleTime) noexcept {
    // RT-SAFE: no allocation, no locks. Device time T is media position
    // (timeline origin + T); playout reads link-offset frames behind it.
    std::memset(outputData, 0, static_cast<size_t>(frameCount) * channelCount * sizeof(float));

    const MediaClock::Snapshot clock = rtInterface_.mediaClock().snapshot();
    if (!clock.valid()) {
        return;
    }
    const int64_t readPosition = clock.origin + static_cast<int64_t>(std::floor(sampleTime)) -
                                 rtInterface_.linkOffsetFrames();

    bool missing = false;
    rtInterface_.rxRouting().read([&](const RxRouting::Route& route) {
        // Each stream fills its own device channels; unrouted channels stay silent
        if (route.buffer->read(readPosition, frameCount, outputData, channelCount, route.deviceChannelStart) <
            frameCount) {
            missing = true;
        }
    });

    if (missing) {
        rtInterface_.recordInputUnderrun();
    }
}

void AES67IOHandler::processOutput(const float* inputData, UInt32 frameCount, UInt32 channelCount) noexcept {
    // RT-SAFE: Write to output ring buffers (Core Audio → Network)

    float channelBuffer[kMaxFramesPerChunk];

    if (frameCount > kMaxFramesPerChunk) {
        return;
    }

    bool hadOverrun = false;
    auto& outputBuffers = rtInterface_.outputBuffers();

    for (size_t ch = 0; ch < channelCount; ++ch) {
        for (UInt32 frame = 0; frame < frameCount; ++frame) {
            channelBuffer[frame] = inputData[frame * channelCount + ch];
        }

        const size_t samplesWritten = outputBuffers[ch].write(channelBuffer, frameCount);

        if (samplesWritten < frameCount) {
            if (!hadOverrun) {
                rtInterface_.recordOutputOverrun();
                hadOverrun = true;
            }
        }
    }
}

} // namespace AES67
