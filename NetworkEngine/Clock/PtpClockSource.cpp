/// @file PtpClockSource.cpp

#include "PtpClockSource.h"
#include "HostTime.h"

namespace AES67 {

PtpClockSource::PtpClockSource(Config config, const MediaClock& clock, const std::atomic<double>& sampleRate,
                               RateWriter writer, ChangeListener listener, OffsetWriter offsetWriter)
    : config_(std::move(config)),
      clock_(clock),
      sampleRate_(sampleRate),
      writer_(std::move(writer)),
      listener_(std::move(listener)),
      offsetWriter_(std::move(offsetWriter)),
      receiver_(config_.receiver),
      control_(config_.control) {}

PtpClockSource::~PtpClockSource() { stop(); }

uint32_t PtpClockSource::clockDomainFor(const Ptp::ClockIdentity& grandmaster, uint8_t domain) {
    // FNV-1a over the identity and domain; 0 means "no domain" to Core Audio
    uint32_t hash = 2166136261u;
    auto mix = [&hash](uint8_t byte) {
        hash ^= byte;
        hash *= 16777619u;
    };
    for (const uint8_t byte : grandmaster.bytes) mix(byte);
    mix(domain);
    return hash == 0 ? 1 : hash;
}

void PtpClockSource::start() {
    if (thread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        stopping_ = false;
    }
    thread_ = std::thread([this] { run(); });
}

void PtpClockSource::stop() {
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    receiver_.stop();
    receiving_.store(false, std::memory_order_release);
    active_.store(false, std::memory_order_release);
    locked_.store(false, std::memory_order_release);
    clockDomain_.store(0, std::memory_order_release);
}

PtpClockSource::Status PtpClockSource::status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void PtpClockSource::run() {
    std::unique_lock<std::mutex> lock(wakeMutex_);
    while (!stopping_) {
        lock.unlock();
        if (!receiving_.load(std::memory_order_relaxed) && receiver_.start()) {
            receiving_.store(true, std::memory_order_release);
        }
        const bool receiving = receiving_.load(std::memory_order_relaxed);
        if (receiving) {
            step();
        }
        lock.lock();
        wake_.wait_for(lock, receiving ? config_.period : config_.retryPeriod, [this] { return stopping_; });
    }
}

void PtpClockSource::step() {
    const Ptp::TimeReceiver::Status received = receiver_.status();
    PtpReference reference;
    reference.following = received.estimate.has_value() && (received.state == Ptp::Servo::State::Locked ||
                                                             received.state == Ptp::Servo::State::Holdover);
    reference.generation = received.generation;
    if (received.estimate) {
        reference.estimate = *received.estimate;
    }

    const MediaClock::Snapshot clock = clock_.snapshot();
    const auto update = control_.update(clock, sampleRate_.load(), hostTimeNow(), reference);
    if (update.offsetChanged && offsetWriter_) {
        offsetWriter_(clock.generation, *control_.offset());
    }
    if (update.samplesPerTick) {
        writer_(clock.generation, *update.samplesPerTick);
    }

    const bool locked = reference.following;
    const uint32_t domain = locked && received.master
        ? clockDomainFor(received.master->announce.grandmasterIdentity, config_.receiver.domain)
        : 0;
    active_.store(control_.active(), std::memory_order_release);
    // Both exchanged unconditionally: either may have changed
    const bool lockChanged = locked_.exchange(locked, std::memory_order_acq_rel) != locked;
    const bool domainChanged = clockDomain_.exchange(domain, std::memory_order_acq_rel) != domain;
    const bool changed = lockChanged || domainChanged;

    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_.receiver = received;
        status_.active = control_.active();
        status_.offset = control_.offset();
        status_.phaseErrorSamples = control_.phaseErrorSamples();
    }
    if (changed && listener_) {
        listener_();
    }
}

} // namespace AES67
