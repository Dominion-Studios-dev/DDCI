#pragma once

#include <chrono>
#include <deque>

namespace ddci::governor {

class RateGovernor {
public:
    struct Limits {
        std::uint32_t rpm;
        std::uint32_t tpm;
    };

    explicit RateGovernor(Limits limits = Limits{30, 60000});

    ~RateGovernor() = default;

    RateGovernor(const RateGovernor&) = delete;
    RateGovernor& operator=(const RateGovernor&) = delete;
    RateGovernor(RateGovernor&&) noexcept = default;
    RateGovernor& operator=(RateGovernor&&) noexcept = default;

    [[nodiscard]] bool is_allowed(std::uint32_t estimated_tokens = 0);

    [[nodiscard]] std::uint64_t wait_time_ms() const noexcept;

    void record(std::uint32_t tokens_used);

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct RequestRecord {
        TimePoint   timestamp;
        std::uint32_t tokens;
    };

    void prune_oldest();

    Limits limits_;
    std::deque<RequestRecord> window_;
};

}
