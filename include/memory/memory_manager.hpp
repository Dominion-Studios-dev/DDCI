#pragma once

#include "core/config.hpp"
#include "core/error.hpp"
#include "core/types.hpp"
#include "storage/db_client.hpp"

#include <cstddef>
#include <vector>

namespace ddci::memory {

class MemoryManager {
public:
    explicit MemoryManager(const core::Config& config,
                           storage::DatabaseClient& db_client);
    ~MemoryManager() = default;

    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    MemoryManager(MemoryManager&&) noexcept = default;
    MemoryManager& operator=(MemoryManager&&) noexcept = default;

    [[nodiscard]] core::Result<void> add_message(
        core::Role role,
        std::string_view content,
        double importance = 1.0);

    [[nodiscard]] std::vector<core::Message> get_context() const;

    [[nodiscard]] core::Result<void> flush_to_storage();

    [[nodiscard]] bool check_invariant() const noexcept;

    [[nodiscard]] size_t get_l1_token_count() const noexcept;

    [[nodiscard]] size_t get_l1_message_count() const noexcept;

    [[nodiscard]] size_t get_l2_staging_count() const noexcept;

private:
    [[nodiscard]] core::Result<void> persist_to_l3(
        const std::vector<core::Message>& evicted);

    const core::Config& config_;
    storage::DatabaseClient& db_;

    std::vector<core::Message> active_buffer_;
    std::vector<core::Message> staging_buffer_;
    size_t current_l1_tokens_{0};

    static constexpr double safety_threshold_ = 0.85;
};

}
