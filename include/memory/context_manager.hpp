#pragma once

#include "core/config.hpp"
#include "core/types.hpp"
#include "core/error.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ddci::memory {

class ContextManager {
public:
    struct Options {
        size_t max_l1_tokens{8192};
        double eviction_threshold{0.80};
        double rearm_threshold{0.60};
        size_t chunk_size{16};
    };

    explicit ContextManager(const core::Config& config);
    ~ContextManager();

    ContextManager(const ContextManager&) = delete;
    ContextManager& operator=(const ContextManager&) = delete;
    ContextManager(ContextManager&&) = delete;
    ContextManager& operator=(ContextManager&&) = delete;

    [[nodiscard]] core::Result<void> init();

    [[nodiscard]] core::Result<void> add_message(core::Role role,
                                                 std::string_view content,
                                                 double importance = 1.0);

    [[nodiscard]] core::Result<void> evict_if_needed();

    [[nodiscard]] core::Result<void> clear() noexcept;

    [[nodiscard]] std::vector<core::Message> get_context() const;

    [[nodiscard]] core::Result<void> flush();

    [[nodiscard]] size_t active_token_count() const noexcept;
    [[nodiscard]] size_t message_count() const noexcept;
    [[nodiscard]] size_t evicted_chunk_count() const noexcept;
    [[nodiscard]] bool is_running() const noexcept;

    static size_t estimate_tokens(std::string_view text) noexcept {
        return (text.length() + 3) / 4;
    }

private:
    struct Chunk {
        std::string category;
        std::string content;
        double importance{0.0};
    };

    void enqueue_chunk(Chunk chunk);
    void worker_main();

    const core::Config& config_;
    Options options_;

    std::vector<core::Message> window_;
    size_t active_tokens_{0};
    std::string system_prompt_;

    std::thread worker_;
    std::mutex q_mutex_;
    std::condition_variable q_cv_;
    std::deque<Chunk> queue_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> worker_ready_{false};
    std::atomic<bool> worker_busy_{false};
    std::atomic<size_t> evicted_chunk_count_{0};
    std::atomic<size_t> evicted_message_count_{0};

    static constexpr std::string_view kSystemPrompt =
        "You are DDCI, a high-performance local AI agent with sliding-window "
        "memory. Respond concisely and precisely. Follow user turns in order.";
};

}
