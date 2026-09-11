#pragma once

#include "core/config.hpp"
#include "core/error.hpp"
#include "core/types.hpp"
#include "memory/arena_allocator.hpp"
#include "network/api_client.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ddci::telemetry {
class UsageTracker;
}

namespace ddci::net {
class EndpointHealth;
class TimingLog;
}

namespace ddci::engine {

class GoTEngine {
public:
    struct Options {
        std::size_t num_branches{3};
        double prune_threshold{0.70};
        double syntax_weight{0.40};
        double constraint_weight{0.40};
        double density_weight{0.20};
        std::size_t arena_capacity{1u << 20};
    };

    struct Node {
        enum class Kind : std::uint8_t {
            Root,
            Branch,
            Aggregate
        };

        Kind kind;
        std::string_view content;
        std::size_t token_count{0};
        std::size_t prompt_tokens{0};
        std::size_t completion_tokens{0};
        double confidence{0.0};
    };

    struct Edge {
        std::size_t from;
        std::size_t to;
    };

    explicit GoTEngine(const core::Config& config,
                       network::APIClient& primary);
    GoTEngine(const core::Config& config,
              network::APIClient& primary,
              Options options);
    ~GoTEngine() = default;

    GoTEngine(const GoTEngine&) = delete;
    GoTEngine& operator=(const GoTEngine&) = delete;
    GoTEngine(GoTEngine&&) = delete;
    GoTEngine& operator=(GoTEngine&&) = delete;

    void set_usage_tracker(telemetry::UsageTracker* tracker) noexcept;
    void set_timing_log(net::TimingLog* log) noexcept;
    void set_endpoint_health(net::EndpointHealth* health) noexcept;
    void set_system_vibe(std::string_view vibe) noexcept;

    [[nodiscard]] core::Result<std::string> run(std::string_view user_prompt);

    [[nodiscard]] const Options& options() const noexcept { return options_; }
    [[nodiscard]] std::span<const Node> nodes() const noexcept;
    [[nodiscard]] std::span<const Edge> edges() const noexcept;
    [[nodiscard]] std::size_t node_count() const noexcept { return node_count_; }
    [[nodiscard]] std::size_t edge_count() const noexcept { return edge_count_; }

private:
    struct Heuristic {
        double syntax{0.0};
        double constraint{0.0};
        double density{0.0};
        double confidence{0.0};
    };

    struct BranchOutcome {
        bool ok{false};
        std::string content;
        nlohmann::json usage;
        std::uint64_t prompt_tokens{0};
        std::uint64_t completion_tokens{0};
    };

    std::string_view stage(std::string_view text);
    Heuristic score(std::string_view prompt, std::string_view candidate);
    void record_branch_usage(const BranchOutcome& outcome);
    void persist_metrics(std::string_view prompt,
                         std::size_t kept, std::size_t pruned,
                         std::int64_t elapsed_ms);

    const core::Config& config_;
    network::APIClient& primary_;
    Options options_;

    telemetry::UsageTracker* tracker_{nullptr};

    memory::ArenaAllocator arena_;
    Node* nodes_{nullptr};
    Edge* edges_{nullptr};
    std::size_t node_count_{0};
    std::size_t edge_count_{0};
    std::size_t node_capacity_{0};
    std::size_t edge_capacity_{0};

    std::list<std::string> owned_overflow_;
    std::vector<std::unique_ptr<network::APIClient>> branch_clients_;

    static constexpr std::string_view kBranchPrompts[3] = {
        "You are a precise problem solver. Think step-by-step, follow the "
        "user's constraints exactly, and give a direct, well-reasoned "
        "answer with no filler.",
        "You are a creative systems thinker. Explore diverse angles and "
        "multiple perspectives, then outline the strongest reasoning path "
        "to a robust conclusion.",
        "You are a rigorous verifier. Reason conservatively, double-check "
        "your logic, explicitly flag any uncertainty, and produce the most "
        "defensible answer."};

    static constexpr std::string_view kSynthesizerPrompt =
        "You are a synthesis engine for a graph-of-thought reasoning system. "
        "Below are candidate solutions produced by different reasoning "
        "strategies, each with a confidence score. Merge the strongest, most "
        "consistent reasoning into ONE polished final answer: concise, "
        "correct, and complete. Do not mention the candidates or the "
        "scoring — answer the original prompt directly.";

    static constexpr std::size_t kMaxSignificantTokens = 16;
};

}
