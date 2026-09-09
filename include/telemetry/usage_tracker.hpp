#pragma once

#include "telemetry/mmap_logger.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ddci::telemetry {

class alignas(64) UsageTracker {
public:
    struct alignas(64) TokenRates {
        double input_per_1k;
        double output_per_1k;
    };

    struct alignas(64) SessionCounters {
        std::uint64_t prompt_tokens;
        std::uint64_t completion_tokens;
        std::uint64_t total_tokens;
        double        cost;
    };

    explicit UsageTracker(const std::string& model,
                          const std::string& csv_path = "ddci_billing.csv",
                          TokenRates rates = TokenRates{0.0001, 0.0002});

    ~UsageTracker() = default;

    UsageTracker(const UsageTracker&) = delete;
    UsageTracker& operator=(const UsageTracker&) = delete;
    UsageTracker(UsageTracker&&) noexcept = default;
    UsageTracker& operator=(UsageTracker&&) noexcept = default;

    void record(const nlohmann::json& response);

    [[nodiscard]] std::uint64_t total_prompt_tokens()     const noexcept;
    [[nodiscard]] std::uint64_t total_completion_tokens() const noexcept;
    [[nodiscard]] std::uint64_t total_tokens()            const noexcept;
    [[nodiscard]] double        total_cost()              const noexcept;

    [[nodiscard]] std::string summary() const;

private:
    void ensure_csv_header();

    void write_timestamp(char* buf, std::size_t cap) const;

    std::string model_;
    std::string csv_path_;
    TokenRates  rates_;

    alignas(64) SessionCounters counters_;

    alignas(64) char ts_buf_[40];

    MmapLogger mmap_logger_;
};

}
