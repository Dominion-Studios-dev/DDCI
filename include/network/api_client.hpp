#pragma once

#include "core/config.hpp"
#include "core/error.hpp"
#include "core/types.hpp"
#include "ddci/net/request_timing.hpp"
#include "memory/arena_allocator.hpp"
#include "memory/simd_cache.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ddci::telemetry {
class UsageTracker;
}

namespace ddci::net {
class ConnectionManager;
class EndpointHealth;
class TimingLog;
}

namespace ddci::network {

class APIClient {
public:
    struct Completion {
        std::string content;
        nlohmann::json usage;
    };

    explicit APIClient(const core::Config& config);
    ~APIClient();

    APIClient(const APIClient&) = delete;
    APIClient& operator=(const APIClient&) = delete;
    APIClient(APIClient&&) noexcept;
    APIClient& operator=(APIClient&&) = delete;

    [[nodiscard]] core::Result<std::string> chat(std::string_view user_message);

    [[nodiscard]] core::Result<std::string> chat_completion(
        const std::vector<core::Message>& messages);

    [[nodiscard]] core::Result<Completion> chat_completion_meta(
        const std::vector<core::Message>& messages);

    [[nodiscard]] core::Result<std::string> chat_completion_stream(
        const std::vector<core::Message>& messages);

    [[nodiscard]] core::Result<std::string> chat_history(
        const std::vector<nlohmann::json>& messages);

    void set_usage_tracker(telemetry::UsageTracker* tracker);
    void set_timing_log(net::TimingLog* logw);
    void set_endpoint_health(net::EndpointHealth* health);
    void set_system_vibe(std::string_view vibe) noexcept;

    [[nodiscard]] const net::RequestTiming& last_request_timing() const noexcept {
        return last_request_timing_;
    }

private:
    [[nodiscard]] nlohmann::json make_chat_payload(
        const std::vector<core::Message>& messages) const;

    void prepend_vibe(nlohmann::json& message_array) const;

    void record_zero_usage();

    void emit_request_timing(net::RequestTiming& t) noexcept;

    [[nodiscard]] core::Result<std::string> post_and_extract(
        nlohmann::json payload);

    [[nodiscard]] core::Result<std::string> post_and_extract_impl(
        nlohmann::json payload, nlohmann::json* usage_out);

    const core::Config& config_;
    telemetry::UsageTracker* tracker_ = nullptr;
    std::string_view vibe_;

    std::unique_ptr<net::ConnectionManager> conn_;

    net::TimingLog* timing_log_ = nullptr;
    net::EndpointHealth* health_ = nullptr;
    net::RequestTiming last_request_timing_;

    memory::ArenaAllocator arena_;

    memory::SemanticCache semantic_cache_;
};

}
