#include "memory/context_manager.hpp"
#include "memory/pruner.hpp"
#include "storage/db_client.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace ddci::memory {

ContextManager::ContextManager(const core::Config& config)
    : config_(config)
    , options_{config.max_l1_tokens} {
    window_.reserve(64);
}

ContextManager::~ContextManager() {
    {
        std::unique_lock<std::mutex> lock(q_mutex_);
        stop_.store(true, std::memory_order_release);
    }
    q_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

core::Result<void> ContextManager::init() {
    system_prompt_ = prune::compress_system_prompt(kSystemPrompt);

    core::Message sys;
    sys.role = core::Role::System;
    sys.content = system_prompt_;
    sys.token_count = estimate_tokens(system_prompt_);
    sys.importance_score = 1.0;
    sys.timestamp = std::chrono::system_clock::now();

    window_.clear();
    window_.push_back(std::move(sys));
    active_tokens_ = window_.front().token_count;

    try {
        worker_ = std::thread(&ContextManager::worker_main, this);
    } catch (const std::system_error& e) {
        std::cerr << "[MEMORY] background L3 writer unavailable: "
                  << e.what() << "\n";
    }

    return core::Result<void>::success();
}

core::Result<void> ContextManager::add_message(core::Role role,
                                               std::string_view content,
                                               double importance) {
    core::Message msg;
    msg.role = role;
    msg.content.assign(content.begin(), content.end());
    msg.token_count = estimate_tokens(content);
    msg.importance_score = importance;
    msg.timestamp = std::chrono::system_clock::now();

    active_tokens_ += msg.token_count;
    window_.push_back(std::move(msg));

    return evict_if_needed();
}

core::Result<void> ContextManager::evict_if_needed() {
    if (window_.size() <= 1) {
        return core::Result<void>::success();
    }

    const size_t trigger = static_cast<size_t>(
        static_cast<double>(options_.max_l1_tokens) *
        options_.eviction_threshold);
    if (active_tokens_ <= trigger) {
        return core::Result<void>::success();
    }

    const size_t target = static_cast<size_t>(
        static_cast<double>(options_.max_l1_tokens) *
        options_.rearm_threshold);

    std::vector<core::Message> evicted;
    evicted.reserve(options_.chunk_size * 2);

    while (active_tokens_ > target && window_.size() > 1) {
        const size_t scan =
            std::min<size_t>(options_.chunk_size, window_.size() - 1);

        size_t pick = 1;
        for (size_t k = 1; k < scan; ++k) {
            if (window_[k].importance_score < window_[pick].importance_score) {
                pick = k;
            }
        }

        active_tokens_ -= window_[pick].token_count;
        evicted.push_back(std::move(window_[pick]));
        window_.erase(window_.begin() + pick);
    }

    if (evicted.empty()) {
        return core::Result<void>::success();
    }

    std::string chunk_content;
    chunk_content.reserve(4096);
    for (const auto& msg : evicted) {
        switch (msg.role) {
            case core::Role::System:    chunk_content += "[system] ";   break;
            case core::Role::User:      chunk_content += "[user] ";     break;
            case core::Role::Assistant: chunk_content += "[assistant] "; break;
            case core::Role::Tool:      chunk_content += "[tool] ";     break;
        }
        chunk_content.append(msg.content);
        chunk_content.push_back('\n');
    }
    chunk_content = prune::compress_context(chunk_content);

    double importance = 0.0;
    for (const auto& msg : evicted) {
        importance += msg.importance_score;
    }
    importance /= static_cast<double>(evicted.size());

    enqueue_chunk(Chunk{std::string("l1_evicted"),
                        std::move(chunk_content),
                        importance});

    evicted_message_count_.fetch_add(evicted.size(),
                                     std::memory_order_relaxed);

    std::cerr << "[MEMORY] sliding window: evicted "
              << evicted.size() << " turns (" << evicted_chunk_count_
              << " chunks), active now "
              << active_tokens_ << "/" << options_.max_l1_tokens
              << " tokens\n";

    evicted.clear();
    return core::Result<void>::success();
}

core::Result<void> ContextManager::clear() noexcept {
    core::Message sys;
    sys.role = core::Role::System;
    sys.content = system_prompt_;
    sys.token_count = estimate_tokens(system_prompt_);
    sys.importance_score = 1.0;
    sys.timestamp = std::chrono::system_clock::now();

    std::vector<core::Message> fresh;
    fresh.reserve(64);
    fresh.push_back(std::move(sys));
    window_.swap(fresh);
    active_tokens_ = window_.front().token_count;

    return core::Result<void>::success();
}

std::vector<core::Message> ContextManager::get_context() const {
    return window_;
}

core::Result<void> ContextManager::flush() {
    std::unique_lock<std::mutex> lock(q_mutex_);

    if (!worker_ready_.load(std::memory_order_acquire) && !queue_.empty()) {
        return core::Result<void>::failure(core::ErrorCode::DatabaseError);
    }

    q_cv_.wait(lock, [this] {
        return queue_.empty() &&
               !worker_busy_.load(std::memory_order_acquire);
    });

    return core::Result<void>::success();
}

bool ContextManager::is_running() const noexcept {
    return !stop_.load(std::memory_order_acquire);
}

size_t ContextManager::active_token_count() const noexcept {
    return active_tokens_;
}

size_t ContextManager::message_count() const noexcept {
    return window_.size();
}

size_t ContextManager::evicted_chunk_count() const noexcept {
    return evicted_chunk_count_.load(std::memory_order_relaxed);
}

void ContextManager::enqueue_chunk(Chunk chunk) {
    {
        std::unique_lock<std::mutex> lock(q_mutex_);
        queue_.push_back(std::move(chunk));
        evicted_chunk_count_.fetch_add(1, std::memory_order_relaxed);
    }
    q_cv_.notify_one();
}

void ContextManager::worker_main() {
    storage::DatabaseClient db(config_.db_path);
    if (!db.is_open()) {
        std::cerr << "[MEMORY] L3 writer: cannot open db at "
                  << config_.db_path << "\n";
        return;
    }

    auto schema = db.init_schema();
    if (!schema.ok()) {
        std::cerr << "[MEMORY] L3 writer: schema init failed\n";
        return;
    }

    worker_ready_.store(true, std::memory_order_release);

    std::unique_lock<std::mutex> lock(q_mutex_);
    for (;;) {
        q_cv_.wait(lock, [this] {
            return stop_.load(std::memory_order_acquire) || !queue_.empty();
        });

        while (!queue_.empty()) {
            Chunk c = std::move(queue_.front());
            queue_.pop_front();

            worker_busy_.store(true, std::memory_order_release);
            lock.unlock();

            auto write_result = db.insert_l2_summary(c.category, c.content,
                                                     c.importance);
            if (!write_result.ok()) {
                std::cerr << "[MEMORY] L3 write failed for chunk\n";
            }

            lock.lock();
            worker_busy_.store(false, std::memory_order_release);
        }

        q_cv_.notify_all();

        if (stop_.load(std::memory_order_acquire)) {
            break;
        }
    }
}

}
