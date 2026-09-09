#include "telemetry/usage_tracker.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace ddci::telemetry {

UsageTracker::UsageTracker(const std::string& model,
                           const std::string& csv_path,
                           TokenRates rates)
    : model_(model)
    , csv_path_(csv_path)
    , rates_(rates)
    , counters_{0, 0, 0, 0.0}
    , mmap_logger_(csv_path) {
    ensure_csv_header();
}

void UsageTracker::ensure_csv_header() {
    if (mmap_logger_.bytes_written() != 0) {
        return;
    }
    static constexpr std::string_view kHeader =
        "Timestamp,Model,Prompt Tokens,Completion Tokens,Total Tokens,"
        "Input Cost (USD),Output Cost (USD),Total Cost (USD)\n";
    (void)mmap_logger_.append(kHeader);
}

void UsageTracker::record(const nlohmann::json& response) {
    std::uint64_t pt = 0;
    std::uint64_t ct = 0;
    std::uint64_t tt = 0;

    if (response.contains("usage") && response["usage"].is_object()) {
        const auto& u = response["usage"];
        pt = u.value("prompt_tokens", std::uint64_t{0});
        ct = u.value("completion_tokens", std::uint64_t{0});
        tt = u.value("total_tokens", pt + ct);
    }

    const double in_cost  = (static_cast<double>(pt) / 1000.0) * rates_.input_per_1k;
    const double out_cost = (static_cast<double>(ct) / 1000.0) * rates_.output_per_1k;
    const double row_cost = in_cost + out_cost;

    counters_.prompt_tokens     += pt;
    counters_.completion_tokens += ct;
    counters_.total_tokens      += tt;
    counters_.cost              += row_cost;

    write_timestamp(ts_buf_, sizeof(ts_buf_));

    char row[512];
    const int n = std::snprintf(
        row, sizeof(row),
        "%s,%s,%llu,%llu,%llu,%.6f,%.6f,%.6f\n",
        ts_buf_, model_.c_str(),
        static_cast<unsigned long long>(pt),
        static_cast<unsigned long long>(ct),
        static_cast<unsigned long long>(tt),
        in_cost, out_cost, row_cost);

    if (n > 0) {
        (void)mmap_logger_.append(std::string_view(row, static_cast<size_t>(n)));
    }
}

std::uint64_t UsageTracker::total_prompt_tokens() const noexcept {
    return counters_.prompt_tokens;
}

std::uint64_t UsageTracker::total_completion_tokens() const noexcept {
    return counters_.completion_tokens;
}

std::uint64_t UsageTracker::total_tokens() const noexcept {
    return counters_.total_tokens;
}

double UsageTracker::total_cost() const noexcept {
    return counters_.cost;
}

std::string UsageTracker::summary() const {
    std::ostringstream oss;
    oss << "--- Session Usage Summary ---\n"
        << "  Prompt Tokens:     " << counters_.prompt_tokens << "\n"
        << "  Completion Tokens: " << counters_.completion_tokens << "\n"
        << "  Total Tokens:      " << counters_.total_tokens << "\n"
        << "  Estimated Cost:    $" << std::fixed
        << std::setprecision(4) << counters_.cost << "\n"
        << "  Billing Ledger:    " << csv_path_ << "\n"
        << "------------------------------";
    return oss.str();
}

void UsageTracker::write_timestamp(char* buf, std::size_t cap) const {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&t, &utc);

    if (cap > 0) {
        buf[0] = '\0';
        buf[cap - 1] = '\0';
    }
    std::strftime(buf, cap, "%Y-%m-%dT%H:%M:%S", &utc);
}

}
