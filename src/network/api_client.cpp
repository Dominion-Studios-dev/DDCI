#include "network/api_client.hpp"

#include "ddci/net/connection_manager.hpp"
#include "ddci/net/endpoint_health.hpp"
#include "ddci/net/prompt_prefix.hpp"
#include "ddci/net/timing_log.hpp"
#include "ddci/core/simd_matcher.hpp"
#include "ddci/mem/arena.hpp"
#include "security/sanitizer.hpp"
#include "telemetry/usage_tracker.hpp"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace ddci::network {

using json = nlohmann::json;

namespace {

std::string_view last_user_query(
    const std::vector<core::Message>& messages) noexcept {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == core::Role::User) {
            return it->content;
        }
    }
    return {};
}

void print_hit_banner(std::int64_t micros) {
    char buf[64];
    const int n = std::snprintf(
        buf, sizeof(buf), "[SEMANTIC CACHE HIT - %.2fms] ",
        static_cast<double>(micros) / 1000.0);
    if (n > 0) {
        std::cout.write(buf, n);
    }
}

void print_upstream_error(int status_code) {
    switch (status_code) {
        case 401:
        case 403:
            std::cerr << "[ERROR] Authentication failed — your API key is "
                         "invalid or was revoked. Check GROQ_API_KEY (or "
                         "OPENAI_API_KEY) and try again.\n";
            break;
        case 429:
            std::cerr << "[ERROR] Rate limit reached. Wait a few seconds "
                         "and try again.\n";
            break;
        default:
            if (status_code >= 500) {
                std::cerr << "[ERROR] The API service is temporarily "
                             "unavailable (HTTP "
                          << status_code << "). Please retry shortly.\n";
            } else {
                std::cerr << "[ERROR] The API rejected the request (HTTP "
                          << status_code << ").\n";
            }
            break;
    }
}

}

APIClient::APIClient(const core::Config& config)
    : config_(config)
    , conn_(std::make_unique<net::ConnectionManager>(config)) {}

APIClient::~APIClient() = default;

APIClient::APIClient(APIClient&&) noexcept = default;

void APIClient::set_timing_log(net::TimingLog* logw) {
    timing_log_ = logw;
}

void APIClient::set_endpoint_health(net::EndpointHealth* health) {
    health_ = health;
}

void APIClient::emit_request_timing(net::RequestTiming& t) noexcept {
    t.model = config_.model_name;
    t.endpoint = conn_ ? conn_->endpoint() : std::string_view{};
    t.finalize();

    last_request_timing_ = t;

    if (timing_log_ != nullptr) {
        timing_log_->record(t);
    }
    if (health_ != nullptr && !t.endpoint.empty()) {
        health_->report(t.endpoint, t.success, t.total_us);
    }
}

core::Result<std::string> APIClient::post_and_extract(json payload) {
    return post_and_extract_impl(std::move(payload), nullptr);
}

core::Result<std::string> APIClient::post_and_extract_impl(
    json payload, json* usage_out) {
    if (config_.groq_api_key.empty()) {
        std::cerr << "[NETWORK ERROR] API key is empty, cannot send request.\n";
        return core::Result<std::string>::failure(core::ErrorCode::NetworkError);
    }

    arena_.reset();

    net::RequestTiming t;
    t.queue_enter = net::RequestTiming::clock::now();
    t.stream = false;

    if (payload.contains("messages") && payload["messages"].is_array()) {
        const net::PrefixDigest pd = net::stable_prompt_prefix(payload["messages"]);
        t.prefix_digest = pd.hash;
        t.prefix_bytes = pd.bytes;
    }

    cpr::Response r = conn_->post(payload, t);

    if (r.error.code != cpr::ErrorCode::OK) {
        emit_request_timing(t);
        std::cerr << "[ERROR] Could not reach the API endpoint: "
                  << r.error.message
                  << ". Check your network connection.\n";
        return core::Result<std::string>::failure(core::ErrorCode::NetworkError);
    }

    if (r.status_code != 200) {
        emit_request_timing(t);
        print_upstream_error(r.status_code);
        return core::Result<std::string>::failure(core::ErrorCode::NetworkError);
    }

    json response;
    try {
        if (r.text.size() <= arena_.remaining()) {
            const auto staged = arena_.alloc_string(r.text);
            response = json::parse(staged.data(),
                                   staged.data() + staged.size());
        } else {
            response = json::parse(r.text);
        }
    } catch (const json::parse_error& e) {
        emit_request_timing(t);
        std::cerr << "[ERROR] Received an unparseable response from the API: "
                  << e.what() << "\n";
        return core::Result<std::string>::failure(core::ErrorCode::NetworkError);
    }

    try {
        std::string content = response.at("choices")
            .at(0)
            .at("message")
            .at("content")
            .get<std::string>();

        if (usage_out != nullptr) {
            *usage_out = (response.contains("usage") && response["usage"].is_object())
                ? response["usage"]
                : json::object();
        }

        if (response.contains("usage") && response["usage"].is_object()) {
            const auto& u = response["usage"];
            if (u.contains("prompt_tokens")) {
                t.prompt_tokens = u["prompt_tokens"].get<std::uint64_t>();
            }
            if (u.contains("completion_tokens")) {
                t.completion_tokens = u["completion_tokens"].get<std::uint64_t>();
            }
        }

        if (tracker_) {
            tracker_->record(response);
        }

        t.success = true;
        t.response_bytes = r.text.size();
        emit_request_timing(t);
        return core::Result<std::string>(std::move(content));
    } catch (const json::exception& e) {
        emit_request_timing(t);
        std::cerr << "[ERROR] Could not extract the response content: "
                  << e.what() << "\n";
        return core::Result<std::string>::failure(core::ErrorCode::NetworkError);
    }
}

core::Result<std::string> APIClient::chat(std::string_view user_message) {
    json payload;
    payload["model"] = config_.model_name;
    payload["messages"] = json::array({
        {{"role", "user"}, {"content", std::string(user_message)}}
    });

    return post_and_extract(std::move(payload));
}

nlohmann::json APIClient::make_chat_payload(
    const std::vector<core::Message>& messages) const {
    json payload;
    payload["model"] = config_.model_name;

    json message_array = json::array();
    for (const auto& msg : messages) {
        std::string role_str;
        switch (msg.role) {
            case core::Role::System:    role_str = "system";    break;
            case core::Role::User:      role_str = "user";      break;
            case core::Role::Assistant: role_str = "assistant"; break;
            case core::Role::Tool:      role_str = "tool";      break;
        }
        message_array.push_back({
            {"role", role_str},
            {"content", msg.content}
        });
    }
    payload["messages"] = std::move(message_array);
    return payload;
}

core::Result<std::string> APIClient::chat_completion(
    const std::vector<core::Message>& messages) {
    const auto t_entry = net::RequestTiming::clock::now();

    const std::string_view query = last_user_query(messages);
    const bool cacheable = !query.empty() &&
                           query.size() <= memory::kMaxKeyChars;
    if (cacheable) {
        alignas(64) float qv[memory::kEmbedDim];
        semantic_cache_.embed(query, qv);
        const auto t0 = std::chrono::steady_clock::now();
        auto hit = semantic_cache_.lookup(qv);
        const auto elapsed_us = std::chrono::duration_cast<
            std::chrono::microseconds>(std::chrono::steady_clock::now() - t0);

        if (hit) {
            print_hit_banner(elapsed_us.count());
            record_zero_usage();

            net::RequestTiming t;
            t.queue_enter = t_entry;
            const auto t_hit = net::RequestTiming::clock::now();
            t.dns_start = t_hit;
            t.ttfb = t_hit;
            t.ttft = t_hit;
            t.request_end = t_hit;
            const net::PrefixDigest pd = net::stable_prompt_prefix(messages);
            t.prefix_digest = pd.hash;
            t.prefix_bytes = pd.bytes;
            t.stream = false;
            t.cache_hit = true;
            t.connection_reused = false;
            t.success = true;
            emit_request_timing(t);

            return core::Result<std::string>(std::move(hit->response));
        }
    }

    auto result = post_and_extract(make_chat_payload(messages));

    if (result.ok() && cacheable &&
        result.value().size() <= memory::kMaxResponseChars) {
        semantic_cache_.store(query, result.value());
    }
    return result;
}

core::Result<APIClient::Completion> APIClient::chat_completion_meta(
    const std::vector<core::Message>& messages) {
    json usage;
    auto content = post_and_extract_impl(make_chat_payload(messages), &usage);
    if (!content.ok()) {
        return core::Result<Completion>::failure(content.error());
    }
    return core::Result<Completion>(Completion{
        std::move(content).value(), std::move(usage)});
}

core::Result<std::string> APIClient::chat_completion_stream(
    const std::vector<core::Message>& messages) {
    if (config_.groq_api_key.empty()) {
        std::cerr << "[NETWORK ERROR] API key is empty, cannot send request.\n";
        return core::Result<std::string>::failure(core::ErrorCode::NetworkError);
    }

    net::RequestTiming t;
    t.queue_enter = net::RequestTiming::clock::now();
    t.stream = true;

    arena_.reset();

    const std::string_view query = last_user_query(messages);
    const bool cacheable = !query.empty() &&
                           query.size() <= memory::kMaxKeyChars;
    if (cacheable) {
        alignas(64) float qv[memory::kEmbedDim];
        semantic_cache_.embed(query, qv);
        const auto t0 = std::chrono::steady_clock::now();
        auto hit = semantic_cache_.lookup(qv);
        const auto elapsed_us = std::chrono::duration_cast<
            std::chrono::microseconds>(std::chrono::steady_clock::now() - t0);

        if (hit) {
            print_hit_banner(elapsed_us.count());
            const std::string safe = ddci::security::sanitize_terminal_output(
                hit->response);
            if (!safe.empty()) {
                std::cout << safe;
            }
            std::cout << std::flush;

            record_zero_usage();

            const auto t_hit = net::RequestTiming::clock::now();
            t.dns_start = t_hit;
            t.ttfb = t_hit;
            t.ttft = t_hit;
            t.request_end = t_hit;
            const net::PrefixDigest pd = net::stable_prompt_prefix(messages);
            t.prefix_digest = pd.hash;
            t.prefix_bytes = pd.bytes;
            t.cache_hit = true;
            t.connection_reused = false;
            t.success = true;
            emit_request_timing(t);

            std::string cached = std::move(hit->response);
            return core::Result<std::string>(std::move(cached));
        }
    }

    json payload = make_chat_payload(messages);
    payload["stream"] = true;
    payload["stream_options"] = {{"include_usage", true}};

    {
        const net::PrefixDigest pd = net::stable_prompt_prefix(payload["messages"]);
        t.prefix_digest = pd.hash;
        t.prefix_bytes = pd.bytes;
    }

    ddci::mem::ArenaWriter buffer(ddci::mem::ArenaScratchpad::current());
    std::string content;

    json usage;
    bool have_usage = false;
    bool stream_done = false;

    const auto on_data = [&](std::string data, intptr_t) -> bool {
        if (t.ttfb.time_since_epoch().count() == 0) {
            t.ttfb = net::RequestTiming::clock::now();
        }
        buffer.append(data);

        std::ptrdiff_t rel;
        while ((rel = ddci::core::simd_find_byte(
                    buffer.view().data(), buffer.size(), '\n')) !=
               static_cast<std::ptrdiff_t>(buffer.size())) {
            const std::size_t nl = static_cast<std::size_t>(rel);
            std::string_view line_v = buffer.view().substr(0, nl);
            buffer.consume(nl + 1);
            if (!line_v.empty() && line_v.back() == '\r') {
                line_v.remove_suffix(1);
            }

            if (line_v.size() < 5 || line_v.compare(0, 5, "data:") != 0) {
                continue;
            }
            const char* pld = line_v.data() + 5;
            std::size_t pld_len = line_v.size() - 5;
            if (pld_len > 0 && pld[0] == ' ') {
                ++pld;
                --pld_len;
            }

            if (stream_done) {
                continue;
            }
            if (pld_len == 6 && std::memcmp(pld, "[DONE]", 6) == 0) {
                stream_done = true;
                continue;
            }

            try {
                const json chunk = json::parse(pld, pld + pld_len);

                if (!have_usage && chunk.contains("usage") &&
                    chunk["usage"].is_object()) {
                    usage = chunk["usage"];
                    have_usage = true;
                }

                const auto choices = chunk.find("choices");
                if (choices == chunk.end() || !choices->is_array() ||
                    choices->empty() || !(*choices)[0].is_object()) {
                    continue;
                }

                const json& first = (*choices)[0];
                const auto delta = first.find("delta");
                if (delta == first.end() || !delta->is_object()) {
                    continue;
                }
                const auto piece_ref = delta->find("content");
                if (piece_ref == delta->end() || !piece_ref->is_string()) {
                    continue;
                }

                const std::string& piece =
                    piece_ref->get_ref<const std::string&>();
                if (piece.empty()) {
                    continue;
                }

                if (t.ttft.time_since_epoch().count() == 0) {
                    t.ttft = net::RequestTiming::clock::now();
                }

                content += piece;

                const std::string safe =
                    ddci::security::sanitize_terminal_output(piece);
                if (!safe.empty()) {
                    std::cout << safe << std::flush;
                }
            } catch (const json::parse_error&) {
            }
        }
        return true;
    };

    cpr::Response r = conn_->post_stream(payload, cpr::WriteCallback{on_data}, t);

    if (r.error.code != cpr::ErrorCode::OK) {
        emit_request_timing(t);
        std::cerr << "[ERROR] Could not reach the API endpoint: "
                  << r.error.message
                  << ". Check your network connection.\n";
        return core::Result<std::string>::failure(core::ErrorCode::NetworkError);
    }

    if (r.status_code != 200) {
        emit_request_timing(t);
        print_upstream_error(r.status_code);
        return core::Result<std::string>::failure(core::ErrorCode::NetworkError);
    }

    if (!stream_done && content.empty()) {
        emit_request_timing(t);
        std::cerr << "[ERROR] The stream ended before any content was "
                     "received.\n";
        return core::Result<std::string>::failure(core::ErrorCode::NetworkError);
    }
    if (!stream_done) {
        std::cerr << "[WARNING] The stream ended early; returning partial "
                     "content.\n";
    }

    if (tracker_) {
        json syn;
        if (have_usage) {
            syn["usage"] = usage;
        } else {
            std::uint64_t pt = 0;
            for (const auto& msg : messages) {
                pt += static_cast<std::uint64_t>(
                    (msg.content.size() + 3) / 4);
            }
            const std::uint64_t ct = static_cast<std::uint64_t>(
                (content.size() + 3) / 4);
            syn["usage"] = {{"prompt_tokens", pt},
                            {"completion_tokens", ct},
                            {"total_tokens", pt + ct}};
        }
        tracker_->record(syn);
    }

    if (have_usage) {
        if (usage.contains("prompt_tokens")) {
            t.prompt_tokens = usage["prompt_tokens"].get<std::uint64_t>();
        }
        if (usage.contains("completion_tokens")) {
            t.completion_tokens = usage["completion_tokens"].get<std::uint64_t>();
        }
    } else {
        t.prompt_tokens = 0;
        t.completion_tokens = 0;
    }

    if (cacheable && content.size() <= memory::kMaxResponseChars) {
        semantic_cache_.store(query, content);
    }

    t.success = true;
    emit_request_timing(t);
    return core::Result<std::string>(std::move(content));
}

core::Result<std::string> APIClient::chat_history(
    const std::vector<nlohmann::json>& messages) {
    json payload;
    payload["model"] = config_.model_name;

    payload["messages"] = messages;

    return post_and_extract(std::move(payload));
}

void APIClient::set_usage_tracker(telemetry::UsageTracker* tracker) {
    tracker_ = tracker;
}

void APIClient::record_zero_usage() {
    if (tracker_ == nullptr) {
        return;
    }
    json syn;
    syn["usage"] = {{"prompt_tokens", 0}, {"completion_tokens", 0},
                    {"total_tokens", 0}};
    tracker_->record(syn);
}

}
