#include "engine/got_engine.hpp"

#include "storage/db_client.hpp"
#include "telemetry/usage_tracker.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <iostream>
#include <string>
#include <utility>

namespace ddci::engine {

using json = nlohmann::json;

namespace {

constexpr std::size_t kMaxSignificant = 16;

constexpr double clamp01(double v) noexcept {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

inline char lower_char(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool is_alnum(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

inline bool lower_eq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower_char(a[i]) != lower_char(b[i])) {
            return false;
        }
    }
    return true;
}

inline bool contains_token_ci(std::string_view content,
                              std::string_view tok) noexcept {
    if (tok.empty() || content.size() < tok.size()) {
        return false;
    }
    for (std::size_t i = 0; i + tok.size() <= content.size(); ++i) {
        bool match = true;
        for (std::size_t k = 0; k < tok.size(); ++k) {
            if (lower_char(content[i + k]) != lower_char(tok[k])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

constexpr std::string_view kStopwords[] = {
    "the", "and", "for", "with", "from", "that", "this", "what", "your",
    "into", "about", "than", "then", "have", "will", "were", "are", "was",
    "been", "most", "some", "very", "just", "when", "where", "which",
    "would", "could", "should", "their", "there", "they", "them", "these",
    "those", "does", "doing", "did", "can", "cant", "not", "its", "each",
    "other", "also", "over", "under", "after", "before", "between"};

inline bool is_stopword(std::string_view tok) noexcept {
    for (const auto& sw : kStopwords) {
        if (lower_eq(sw, tok)) {
            return true;
        }
    }
    return false;
}

double syntax_score(std::string_view s) noexcept {
    if (s.empty()) {
        return 0.0;
    }
    int32_t balance = 0;
    bool broken = false;
    for (char c : s) {
        switch (c) {
            case '(': case '[': case '{': ++balance; break;
            case ')': case ']': case '}':
                --balance;
                if (balance < 0) {
                    broken = true;
                    balance = 0;
                }
                break;
            default: break;
        }
    }

    double sc = (!broken && balance == 0) ? 1.0 : 0.5;
    const unsigned char last = static_cast<unsigned char>(s.back());
    if (last == '.' || last == '!' || last == '?' || last == '"' ||
        last == '\'' || last == ')' || last == ']' || last == '}') {
        sc += 0.2;
    } else {
        sc -= 0.2;
    }
    return clamp01(sc);
}

double constraint_score(std::string_view content,
                        std::string_view prompt) noexcept {
    std::array<std::string_view, kMaxSignificant> toks{};
    std::size_t n = 0;

    const char* p = prompt.data();
    const std::size_t pl = prompt.size();
    std::size_t i = 0;
    while (i < pl && n < toks.size()) {
        if (!is_alnum(p[i])) {
            ++i;
            continue;
        }
        const std::size_t start = i;
        while (i < pl && is_alnum(p[i])) {
            ++i;
        }
        const std::string_view tok = prompt.substr(start, i - start);
        if (tok.size() < 4 || is_stopword(tok)) {
            continue;
        }
        bool duplicate = false;
        for (std::size_t k = 0; k < n; ++k) {
            if (lower_eq(toks[k], tok)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            toks[n++] = tok;
        }
    }

    if (n == 0) {
        return 1.0;
    }

    std::size_t hit = 0;
    for (std::size_t k = 0; k < n; ++k) {
        if (contains_token_ci(content, toks[k])) {
            ++hit;
        }
    }
    return static_cast<double>(hit) / static_cast<double>(n);
}

double density_score(std::string_view content,
                     std::string_view prompt) noexcept {
    const std::size_t ct = (content.size() + 3) / 4;
    if (ct == 0) {
        return 0.0;
    }
    const std::size_t pt = std::max<std::size_t>(1, (prompt.size() + 3) / 4);
    const double target = std::max(24.0, static_cast<double>(pt) * 0.8);

    const double complete = clamp01(static_cast<double>(ct) / target);
    const double verbosity =
        (ct > static_cast<std::size_t>(target * 4.0)) ? 0.8 : 1.0;
    return complete * verbosity;
}

}

GoTEngine::GoTEngine(const core::Config& config,
                     network::APIClient& primary)
    : GoTEngine(config, primary, Options{}) {}

GoTEngine::GoTEngine(const core::Config& config,
                     network::APIClient& primary,
                     Options options)
    : config_(config)
    , primary_(primary)
    , options_(std::move(options))
    , arena_(options_.arena_capacity) {
    if (options_.num_branches == 0) {
        options_.num_branches = 1;
    }

    node_capacity_ = options_.num_branches + 2;
    edge_capacity_ = options_.num_branches * 2;

    branch_clients_.reserve(options_.num_branches);
    for (std::size_t b = 0; b < options_.num_branches; ++b) {
        branch_clients_.emplace_back(
            std::make_unique<network::APIClient>(config_));
    }
}

void GoTEngine::set_usage_tracker(telemetry::UsageTracker* tracker) noexcept {
    tracker_ = tracker;
}

void GoTEngine::set_timing_log(net::TimingLog* log) noexcept {
    primary_.set_timing_log(log);
    for (const auto& client : branch_clients_) {
        client->set_timing_log(log);
    }
}

void GoTEngine::set_endpoint_health(net::EndpointHealth* health) noexcept {
    primary_.set_endpoint_health(health);
    for (const auto& client : branch_clients_) {
        client->set_endpoint_health(health);
    }
}

std::span<const GoTEngine::Node> GoTEngine::nodes() const noexcept {
    return nodes_ != nullptr ? std::span<const Node>(nodes_, node_count_)
                             : std::span<const Node>{};
}

std::span<const GoTEngine::Edge> GoTEngine::edges() const noexcept {
    return edges_ != nullptr ? std::span<const Edge>(edges_, edge_count_)
                             : std::span<const Edge>{};
}

std::string_view GoTEngine::stage(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    std::string_view view = arena_.alloc_string(text);
    if (view.data() == nullptr) {
        owned_overflow_.emplace_back(text);
        view = owned_overflow_.back();
    }
    return view;
}

GoTEngine::Heuristic GoTEngine::score(std::string_view prompt,
                                      std::string_view candidate) {
    Heuristic h;
    h.syntax = syntax_score(candidate);
    h.constraint = constraint_score(candidate, prompt);
    h.density = density_score(candidate, prompt);
    h.confidence = clamp01(options_.syntax_weight * h.syntax +
                           options_.constraint_weight * h.constraint +
                           options_.density_weight * h.density);
    return h;
}

void GoTEngine::record_branch_usage(const BranchOutcome& outcome) {
    if (tracker_ == nullptr || !outcome.ok || !outcome.usage.is_object()) {
        return;
    }
    json syn;
    syn["usage"] = outcome.usage;
    tracker_->record(syn);
}

void GoTEngine::persist_metrics(std::string_view prompt,
                                std::size_t kept,
                                std::size_t pruned,
                                std::int64_t elapsed_ms) {
    storage::DatabaseClient db(config_.db_path);
    if (!db.is_open()) {
        std::cerr << "[GOT] metrics db unavailable at " << config_.db_path
                  << "\n";
        return;
    }
    (void)db.init_schema();

    json m;
    m["engine"] = "got";
    m["prompt"] = std::string(prompt.substr(0, 256));
    m["num_branches"] = options_.num_branches;
    m["prune_threshold"] = options_.prune_threshold;
    m["kept"] = kept;
    m["pruned"] = pruned;
    m["elapsed_ms"] = elapsed_ms;

    json nodes_arr = json::array();
    double best = 0.0;
    const std::span<const Node> graph_nodes = nodes();
    for (std::size_t idx = 0; idx < graph_nodes.size(); ++idx) {
        const Node& nd = graph_nodes[idx];
        std::string kind;
        switch (nd.kind) {
            case Node::Kind::Root:      kind = "root";      break;
            case Node::Kind::Branch:    kind = "branch";    break;
            case Node::Kind::Aggregate: kind = "aggregate"; break;
        }
        nodes_arr.push_back({
            {"index", idx}, {"kind", kind},
            {"prompt_tokens", nd.prompt_tokens},
            {"completion_tokens", nd.completion_tokens},
            {"token_count", nd.token_count},
            {"confidence", nd.confidence}
        });
        if (nd.kind != Node::Kind::Root && nd.confidence > best) {
            best = nd.confidence;
        }
    }
    json edges_arr = json::array();
    for (const Edge& e : edges()) {
        edges_arr.push_back({{"from", e.from}, {"to", e.to}});
    }
    m["graph"] = {{"nodes", std::move(nodes_arr)},
                  {"edges", std::move(edges_arr)}};
    m["best_confidence"] = best;

    json cands = json::array();
    for (std::size_t idx = 0; idx < graph_nodes.size(); ++idx) {
        std::string_view c = graph_nodes[idx].content;
        cands.push_back(c.substr(0, c.size() <= 512 ? c.size() : 512));
    }
    m["candidates"] = std::move(cands);

    (void)db.insert_l2_summary("got_metrics", m.dump(), best);
}

core::Result<std::string> GoTEngine::run(std::string_view user_prompt) {
    const std::size_t B = options_.num_branches;
    if (user_prompt.empty()) {
        std::cerr << "[GOT] empty prompt.\n";
        return core::Result<std::string>::failure(core::ErrorCode::ConfigError);
    }

    const std::string prompt(user_prompt);

    arena_.reset();
    owned_overflow_.clear();
    nodes_ = static_cast<Node*>(
        arena_.alloc(sizeof(Node) * node_capacity_, alignof(Node)));
    edges_ = static_cast<Edge*>(
        arena_.alloc(sizeof(Edge) * edge_capacity_, alignof(Edge)));
    if (nodes_ == nullptr || edges_ == nullptr) {
        std::cerr << "[GOT] arena cannot back the DAG.\n";
        return core::Result<std::string>::failure(
            core::ErrorCode::MemoryError);
    }
    node_count_ = 0;
    edge_count_ = 0;

    {
        Node& root = nodes_[0];
        root = Node{};
        root.kind = Node::Kind::Root;
        root.content = stage(prompt);
        root.token_count = (prompt.size() + 3) / 4;
        root.confidence = 1.0;
        node_count_ = 1;
    }

    const auto t_start = std::chrono::steady_clock::now();

    std::vector<std::future<BranchOutcome>> futures;
    futures.reserve(B);
    const auto worker =
        [](network::APIClient* client, std::string variation,
           std::string branch_prompt) -> BranchOutcome {
        BranchOutcome out;
        try {
            std::vector<core::Message> msgs;
            msgs.reserve(2);
            core::Message sys;
            sys.role = core::Role::System;
            sys.content = std::move(variation);
            msgs.push_back(std::move(sys));
            core::Message usr;
            usr.role = core::Role::User;
            usr.content = std::move(branch_prompt);
            msgs.push_back(std::move(usr));

            auto res = client->chat_completion_meta(msgs);
            if (!res.ok()) {
                return out;
            }
            out.ok = true;
            auto val = std::move(res).value();
            out.content = std::move(val.content);
            out.usage = std::move(val.usage);
            if (out.usage.is_object()) {
                out.prompt_tokens =
                    out.usage.value("prompt_tokens", out.prompt_tokens);
                out.completion_tokens =
                    out.usage.value("completion_tokens", out.completion_tokens);
            }
            if (out.prompt_tokens == 0) {
                out.prompt_tokens =
                    (msgs[0].content.size() + msgs[1].content.size() + 3) / 4;
            }
        } catch (...) {
            out.ok = false;
        }
        return out;
    };

    for (std::size_t b = 0; b < B; ++b) {
        try {
            futures.emplace_back(std::async(
                std::launch::async, worker, branch_clients_[b].get(),
                std::string(kBranchPrompts[b % 3]), prompt));
        } catch (const std::system_error&) {
            futures.emplace_back(std::async(
                std::launch::deferred,
                [&worker, &b, &prompt, this]() {
                    return worker(branch_clients_[b].get(),
                                  std::string(kBranchPrompts[b % 3]),
                                  prompt);
                }));
        }
    }

    std::size_t kept = 0;
    std::size_t pruned = 0;
    double best_conf = 0.0;
    std::size_t best_idx = 0;

    for (std::size_t b = 0; b < B; ++b) {
        BranchOutcome out = std::move(futures[b]).get();
        const std::size_t idx = b + 1;

        Node& node = nodes_[idx];
        node = Node{};
        node.kind = Node::Kind::Branch;
        node.content = stage(out.content);
        node.token_count = (out.content.size() + 3) / 4;
        node.prompt_tokens = out.prompt_tokens;
        node.completion_tokens = out.completion_tokens;

        Heuristic h = score(prompt, out.content);
        node.confidence = h.confidence;

        edges_[edge_count_++] = {0, idx};

        record_branch_usage(out);

        if (h.confidence >= options_.prune_threshold) {
            ++kept;
        } else {
            ++pruned;
        }
        if (h.confidence > best_conf) {
            best_conf = h.confidence;
            best_idx = idx;
        }
        node_count_ = idx + 1;
    }

    std::string synthesis;
    synthesis.reserve(4096);
    synthesis += "Original prompt:\n";
    synthesis += prompt;
    synthesis += "\n\nCandidate solutions:\n";

    double agg_confidence = 0.0;
    std::size_t used = 0;
    const std::size_t agg_idx = B + 1;
    const double threshold = options_.prune_threshold;

    for (std::size_t b = 0; b < B; ++b) {
        const Node& nd = nodes_[b + 1];
        if (nd.confidence < threshold || nd.content.empty()) {
            continue;
        }
        if (nd.kind != Node::Kind::Branch) {
            continue;
        }
        synthesis += "--- Candidate ";
        synthesis += std::to_string(b + 1);
        synthesis += " (confidence ";
        char cbuf[32];
        std::snprintf(cbuf, sizeof(cbuf), "%.2f", nd.confidence);
        synthesis += cbuf;
        synthesis += ") ---\n";
        synthesis.append(nd.content);
        synthesis += "\n\n";
        edges_[edge_count_++] = {b + 1, agg_idx};
        agg_confidence += nd.confidence;
        ++used;
    }

    if (used == 0 && best_conf > 0.0) {
        const Node& nd = nodes_[best_idx];
        if (!nd.content.empty()) {
            synthesis += "--- Candidate ";
            synthesis += std::to_string(best_idx);
            synthesis += " (confidence ";
            char cbuf[32];
            std::snprintf(cbuf, sizeof(cbuf), "%.2f", nd.confidence);
            synthesis += cbuf;
            synthesis += ") ---\n";
            synthesis.append(nd.content);
            synthesis += "\n";
            edges_[edge_count_++] = {best_idx, agg_idx};
            agg_confidence = nd.confidence;
            ++used;
        }
    }

    if (used == 0) {
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                       t_start).count();
        persist_metrics(prompt, kept, pruned, elapsed);
        std::cerr << "[GOT] all branches failed or produced no usable "
                     "candidate.\n";
        return core::Result<std::string>::failure(
            core::ErrorCode::NetworkError);
    }

    Node& agg = nodes_[agg_idx];
    agg = Node{};
    agg.kind = Node::Kind::Aggregate;
    agg.confidence = agg_confidence / static_cast<double>(used);

    std::vector<core::Message> agg_msgs;
    agg_msgs.reserve(2);
    core::Message sys;
    sys.role = core::Role::System;
    sys.content = std::string(kSynthesizerPrompt);
    agg_msgs.push_back(std::move(sys));
    core::Message usr;
    usr.role = core::Role::User;
    usr.content = std::move(synthesis);
    agg_msgs.push_back(std::move(usr));

    std::cout << "GoT > " << std::flush;
    auto final_res = primary_.chat_completion_stream(agg_msgs);
    if (!final_res.ok()) {
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                       t_start).count();
        persist_metrics(prompt, kept, pruned, elapsed);
        return core::Result<std::string>::failure(final_res.error());
    }

    std::string final_content = std::move(final_res).value();
    agg.content = stage(final_content);
    agg.token_count = (final_content.size() + 3) / 4;
    agg.completion_tokens = agg.token_count;
    for (std::size_t i = 1; i <= B; ++i) {
        agg.prompt_tokens += nodes_[i].prompt_tokens;
    }
    node_count_ = agg_idx + 1;

    const auto elapsed = std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                   t_start).count();
    persist_metrics(prompt, kept, pruned, elapsed);

    std::cout << "\n[GOT] kept " << kept << "/" << B << " branches, best "
              << std::string(std::to_string(static_cast<int>(best_conf * 100)))
              << "% confidence, " << elapsed << " ms\n\n";

    return core::Result<std::string>(std::move(final_content));
}

}
