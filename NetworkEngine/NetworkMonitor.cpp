/// @file NetworkMonitor.cpp

#include "NetworkMonitor.h"
#include <utility>
#include <vector>

namespace AES67 {

NetworkMonitor::NetworkMonitor(Config config, ChangeHandler handler)
    : config_(std::move(config))
    , handler_(std::move(handler))
    , thread_([this] { run(); })
{
}

NetworkMonitor::~NetworkMonitor() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::optional<NetworkInterfaceDetection::InterfaceState> NetworkMonitor::stateOf(const std::string& setting) const {
    return config_.provider ? config_.provider(setting) : NetworkInterfaceDetection::currentState(setting);
}

void NetworkMonitor::watch(const std::string& setting) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (watched_.find(setting) == watched_.end()) {
        watched_[setting] = stateOf(setting);
    }
}

void NetworkMonitor::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        wake_.wait_for(lock, config_.interval, [this] { return stopping_; });
        if (stopping_) {
            break;
        }

        // Look up states without the lock (getifaddrs), then record changes
        std::vector<std::string> settings;
        for (const auto& entry : watched_) {
            settings.push_back(entry.first);
        }
        lock.unlock();
        std::vector<std::pair<std::string, std::optional<NetworkInterfaceDetection::InterfaceState>>> current;
        for (const auto& setting : settings) {
            current.emplace_back(setting, stateOf(setting));
        }
        lock.lock();

        std::vector<std::string> changed;
        for (auto& [setting, state] : current) {
            auto it = watched_.find(setting);
            if (it != watched_.end() && it->second != state) {
                it->second = std::move(state);
                changed.push_back(setting);
            }
        }

        // Report without the lock: the handler may call watch()
        lock.unlock();
        for (const auto& setting : changed) {
            handler_(setting);
        }
        lock.lock();
    }
}

} // namespace AES67
