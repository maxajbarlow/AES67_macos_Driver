/// @file RxRouting.h
/// @brief The IO thread's lock-free view of which receive buffers feed which device channels.

#pragma once

#include "../Clock/TimestampedAudioBuffer.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace AES67 {

/// Fixed table of routes from a stream's TimestampedAudioBuffer to a block of
/// device input channels. The IO thread reads it without locks; receivers
/// publish and unpublish their buffer from non-real-time threads.
///
/// Reclamation: unpublish() returns only once no read that could have seen
/// the route is still running, so the caller may then free the buffer.
/// Publishers must be serialised by the caller (StreamManager's mutex).
class RxRouting {
public:
    static constexpr size_t kMaxRoutes = 64;
    static constexpr uint32_t kDeviceChannels = 128;

    struct Route {
        const TimestampedAudioBuffer* buffer;
        uint32_t deviceChannelStart;
    };

    /// @return false if the table is full or the channels do not fit the device.
    bool publish(const TimestampedAudioBuffer* buffer, uint32_t deviceChannelStart) noexcept {
        if (!buffer || deviceChannelStart + buffer->channels() > kDeviceChannels) {
            return false;
        }
        for (auto& slot : slots_) {
            if (slot.buffer.load(std::memory_order_relaxed) == nullptr) {
                slot.deviceChannelStart.store(deviceChannelStart, std::memory_order_relaxed);
                slot.buffer.store(buffer, std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    /// Remove `buffer`'s route and wait until no in-flight read can use it.
    void unpublish(const TimestampedAudioBuffer* buffer) noexcept {
        for (auto& slot : slots_) {
            if (slot.buffer.load(std::memory_order_relaxed) == buffer) {
                slot.buffer.store(nullptr, std::memory_order_seq_cst);
            }
        }
        // Reads that started before the removal finish within one IO cycle
        const uint64_t started = readsStarted_.load(std::memory_order_seq_cst);
        while (readsFinished_.load(std::memory_order_seq_cst) < started) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    /// Real-time safe: visit every published route.
    template<typename Visitor>
    void read(Visitor&& visit) const noexcept {
        readsStarted_.fetch_add(1, std::memory_order_seq_cst);
        for (const auto& slot : slots_) {
            const TimestampedAudioBuffer* buffer = slot.buffer.load(std::memory_order_seq_cst);
            if (buffer) {
                visit(Route{buffer, slot.deviceChannelStart.load(std::memory_order_relaxed)});
            }
        }
        readsFinished_.fetch_add(1, std::memory_order_seq_cst);
    }

private:
    struct Slot {
        std::atomic<const TimestampedAudioBuffer*> buffer{nullptr};
        std::atomic<uint32_t> deviceChannelStart{0};
    };

    std::array<Slot, kMaxRoutes> slots_;
    mutable std::atomic<uint64_t> readsStarted_{0};
    mutable std::atomic<uint64_t> readsFinished_{0};
};

} // namespace AES67
