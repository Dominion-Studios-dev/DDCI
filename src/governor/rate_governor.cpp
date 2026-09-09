#include "governor/rate_governor.hpp"

#include <algorithm>

namespace ddci::governor {

RateGovernor::RateGovernor(Limits limits)
    : limits_(limits) {}

void RateGovernor::prune_oldest() {
    const auto cutoff = Clock::now() - std::chrono::seconds(60);
    while (!window_.empty() && window_.front().timestamp < cutoff) {
        window_.pop_front();
    }
}

bool RateGovernor::is_allowed(std::uint32_t estimated_tokens) {
    prune_oldest();

    if (window_.size() >= limits_.rpm) {
        return false;
    }

    std::uint32_t current_tpm = 0;
    for (const auto& rec : window_) {
        current_tpm += rec.tokens;
    }
    if (current_tpm + estimated_tokens > limits_.tpm) {
        return false;
    }

    return true;
}

std::uint64_t RateGovernor::wait_time_ms() const noexcept {
    if (window_.empty()) {
        return 0;
    }

    const auto oldest = window_.front().timestamp;
    const auto now = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - oldest);

    const std::int64_t wait = 60000 - elapsed.count();
    return static_cast<std::uint64_t>(std::max(std::int64_t{0}, wait));
}

void RateGovernor::record(std::uint32_t tokens_used) {
    window_.push_back({Clock::now(), tokens_used});
}

}
