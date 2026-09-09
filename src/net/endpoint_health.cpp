#include "ddci/net/endpoint_health.hpp"

#include <cpr/cpr.h>

#include <algorithm>
#include <chrono>

namespace ddci::net {

EndpointHealth::EndpointHealth(std::string primary,
                               std::chrono::milliseconds interval)
    : primary_(std::move(primary))
    , interval_(interval) {}

EndpointHealth::~EndpointHealth() {
    stop();
}

void EndpointHealth::set_auth_bearer(std::string token) {
    api_key_ = std::move(token);
}

std::vector<std::string> EndpointHealth::endpoints() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(windows_.size() + 1);
    out.push_back(primary_);
    for (const auto& [url, unused] : windows_) {
        if (url != primary_) {
            out.push_back(url);
        }
    }
    return out;
}

void EndpointHealth::report(std::string_view url, bool ok,
                            std::int64_t latency_us) {
    const std::string key(url);
    std::lock_guard<std::mutex> lock(mutex_);
    Window& w = windows_[key];
    w.samples.push_back(Entry{std::max<std::int64_t>(0, latency_us),
                              ok ? std::uint8_t{0} : std::uint8_t{1}});
    w.sum_us += static_cast<std::uint64_t>(std::max<std::int64_t>(0, latency_us));
    if (!ok) {
        ++w.errs;
    }
    if (w.samples.size() > kWindowSamples) {
        const Entry& old = w.samples.front();
        w.sum_us -= static_cast<std::uint64_t>(old.latency_us);
        if (old.failed != 0u) {
            --w.errs;
        }
        w.samples.pop_front();
    }
}

EndpointHealth::Stats EndpointHealth::stats(std::string_view url) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = windows_.find(std::string(url));
    if (it == windows_.end()) {
        return Stats{};
    }
    const Window& w = it->second;
    Stats s;
    s.samples = w.samples.size();
    if (s.samples > 0) {
        s.avg_latency_us = static_cast<std::uint64_t>(w.sum_us / s.samples);
        s.error_rate = static_cast<double>(w.errs) / static_cast<double>(s.samples);
    }
    return s;
}

EndpointHealth::Status EndpointHealth::status(std::string_view url) const {
    const Stats s = stats(url);
    if (s.samples == 0) {
        return Status::Healthy;
    }
    const bool many_errors =
        s.error_rate >= 0.5;
    if (many_errors) {
        return Status::Unhealthy;
    }
    if (s.error_rate > 0.0) {
        return Status::Degraded;
    }
    if (s.avg_latency_us > kSlowLatencyUs) {
        return Status::Degraded;
    }
    return Status::Healthy;
}

void EndpointHealth::check_once(const std::string& url) noexcept {
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = false;
    try {
        cpr::Response r;
        if (api_key_.empty()) {
            r = cpr::Head(cpr::Url{url}, cpr::Timeout{std::chrono::milliseconds(kProbeTimeoutMs)});
        } else {
            r = cpr::Head(cpr::Url{url},
                          cpr::Header{{"Authorization", "Bearer " + api_key_}},
                          cpr::Timeout{std::chrono::milliseconds(kProbeTimeoutMs)});
        }
        ok = (r.error.code == cpr::ErrorCode::OK);
    } catch (...) {
        ok = false;
    }
    const auto latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0)
                                .count();
    report(url, ok, latency_us);
}

void EndpointHealth::monitor_loop() noexcept {
    while (true) {
        {
            std::unique_lock<std::mutex> lk(cv_mutex_);
            if (cv_.wait_for(lk, interval_, [this] {
                    return stop_requests_.load(std::memory_order_acquire);
                })) {
                break;
            }
        }
        for (const std::string& url : endpoints()) {
            if (stop_requests_.load(std::memory_order_acquire)) {
                return;
            }
            check_once(url);
        }
    }
}

void EndpointHealth::start() {
    if (interval_ <= std::chrono::milliseconds(0)) {
        return;
    }
    std::lock_guard<std::mutex> lock(cv_mutex_);
    if (started_) {
        return;
    }
    stop_requests_.store(false, std::memory_order_release);
    monitor_ = std::thread(&EndpointHealth::monitor_loop, this);
    started_ = true;
}

void EndpointHealth::stop() {
    {
        std::lock_guard<std::mutex> lock(cv_mutex_);
        if (!started_) {
            return;
        }
        stop_requests_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    if (monitor_.joinable()) {
        monitor_.join();
    }
    started_ = false;
}

bool EndpointHealth::monitor_running() const noexcept {
    return started_ && monitor_.joinable();
}

}
