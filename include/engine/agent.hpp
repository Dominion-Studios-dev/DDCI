#pragma once

#include "core/config.hpp"
#include "core/error.hpp"
#include "memory/memory_manager.hpp"
#include "network/api_client.hpp"
#include "storage/db_client.hpp"

namespace ddci::engine {

class Agent {
public:
    Agent(core::Config& config,
          storage::DatabaseClient& db,
          memory::MemoryManager& memory,
          network::APIClient& network);
    ~Agent() = default;

    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    Agent(Agent&&) noexcept = default;
    Agent& operator=(Agent&&) noexcept = default;

    [[nodiscard]] core::Result<void> init();
    [[nodiscard]] core::Result<void> run_repl();

private:
    core::Config& config_;
    storage::DatabaseClient& db_;
    memory::MemoryManager& memory_;
    network::APIClient& network_;

    bool is_running_{false};

    static constexpr const char* kSystemPrompt =
        "You are DDCI, the Dominion Distributed Control Interface — a "
        "high-performance local AI agent. You respond concisely, precisely, "
        "and helpfully. You have persistent memory of the conversation.";
};

}
