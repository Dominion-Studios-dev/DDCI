#include "memory/memory_manager.hpp"
#include "memory/token_utils.hpp"

#include <utility>

namespace ddci::memory {

MemoryManager::MemoryManager(const core::Config& config,
                             storage::DatabaseClient& db_client)
    : config_(config)
    , db_(db_client) {
    active_buffer_.reserve(64);
    staging_buffer_.reserve(64);
}

core::Result<void> MemoryManager::add_message(core::Role role,
                                               std::string_view content,
                                               double importance) {
    core::Message msg;
    msg.role = role;
    msg.content = std::string(content);
    msg.token_count = estimate_tokens(content);
    msg.importance_score = importance;
    msg.timestamp = std::chrono::system_clock::now();

    current_l1_tokens_ += msg.token_count;
    active_buffer_.push_back(std::move(msg));

    if (!check_invariant()) {
        auto flush_result = flush_to_storage();
        if (!flush_result.ok()) {
            return flush_result;
        }
    }

    return core::Result<void>::success();
}

std::vector<core::Message> MemoryManager::get_context() const {
    return active_buffer_;
}

core::Result<void> MemoryManager::flush_to_storage() {
    if (active_buffer_.empty()) {
        return core::Result<void>::success();
    }

    const size_t token_budget = static_cast<size_t>(
        config_.max_l1_tokens * safety_threshold_);

    std::vector<core::Message> evicted;
    evicted.reserve(16);

    while (current_l1_tokens_ > token_budget && !active_buffer_.empty()) {
        current_l1_tokens_ -= active_buffer_.front().token_count;
        evicted.push_back(std::move(active_buffer_.front()));
        active_buffer_.erase(active_buffer_.begin());
    }

    if (evicted.empty()) {
        return core::Result<void>::success();
    }

    for (auto& msg : evicted) {
        staging_buffer_.push_back(std::move(msg));
    }

    auto persist_result = persist_to_l3(evicted);
    if (!persist_result.ok()) {
        return persist_result;
    }

    return core::Result<void>::success();
}

core::Result<void> MemoryManager::persist_to_l3(
    const std::vector<core::Message>& evicted) {
    auto prep_result = db_.prepare(
        "INSERT INTO l2_summaries (category, content, importance_score) "
        "VALUES (?, ?, ?)");
    if (!prep_result.ok()) {
        return core::Result<void>::failure(prep_result.error());
    }

    auto stmt = std::move(prep_result).value();

    for (const auto& msg : evicted) {
        auto reset_result = stmt.reset();
        if (!reset_result.ok()) {
            return reset_result;
        }

        std::string category;
        switch (msg.role) {
            case core::Role::System:    category = "system";    break;
            case core::Role::User:      category = "user";      break;
            case core::Role::Assistant: category = "assistant"; break;
            case core::Role::Tool:      category = "tool";      break;
        }

        auto b1 = stmt.bind_text(1, category);
        if (!b1.ok()) return b1;

        auto b2 = stmt.bind_text(2, msg.content);
        if (!b2.ok()) return b2;

        auto b3 = stmt.bind_double(3, msg.importance_score);
        if (!b3.ok()) return b3;

        auto step_result = stmt.step();
        if (!step_result.ok()) {
            return core::Result<void>::failure(step_result.error());
        }
    }

    return core::Result<void>::success();
}

bool MemoryManager::check_invariant() const noexcept {
    const size_t token_budget = static_cast<size_t>(
        config_.max_l1_tokens * safety_threshold_);
    return current_l1_tokens_ <= token_budget;
}

size_t MemoryManager::get_l1_token_count() const noexcept {
    return current_l1_tokens_;
}

size_t MemoryManager::get_l1_message_count() const noexcept {
    return active_buffer_.size();
}

size_t MemoryManager::get_l2_staging_count() const noexcept {
    return staging_buffer_.size();
}

}
