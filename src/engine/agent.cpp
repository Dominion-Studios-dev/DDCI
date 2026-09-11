#include "engine/agent.hpp"

#include "workspace/workspace_context.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace ddci::engine {

Agent::Agent(core::Config& config,
             storage::DatabaseClient& db,
             memory::MemoryManager& memory,
             network::APIClient& network)
    : config_(config)
    , db_(db)
    , memory_(memory)
    , network_(network) {}

core::Result<void> Agent::init() {
    auto schema = db_.init_schema();
    if (!schema.ok()) {
        std::cerr << "[ENGINE] Failed to initialize database schema: "
                  << static_cast<int>(schema.error()) << "\n";
        return schema;
    }

    std::string prompt(kSystemPrompt);
    auto name = db_.get_user_name();
    if (name.ok() && !name.value().empty()) {
        prompt += " The user's name is ";
        prompt += name.value();
        prompt += " — always address them by name.";
    }

    const workspace::GitState git_state = workspace::detect_git_state();
    if (git_state.present) {
        prompt += " Workspace telemetry (git): branch '";
        prompt += git_state.branch;
        prompt += "', ";
        prompt += std::to_string(git_state.modified);
        prompt += " modified, ";
        prompt += std::to_string(git_state.untracked);
        prompt += " untracked.";
    }

    auto history = db_.get_recent_history(20);
    if (history.ok()) {
        for (const auto& msg : history.value()) {
            const core::Role role = (msg.role == "assistant")
                                        ? core::Role::Assistant
                                        : core::Role::User;
            if (!memory_.add_message(role, msg.content, 1.0).ok()) {
                break;
            }
        }
    }

    auto result = memory_.add_message(core::Role::System, prompt, 1.0);
    if (!result.ok()) {
        std::cerr << "[ENGINE] Failed to seed system prompt: "
                  << static_cast<int>(result.error()) << "\n";
        return result;
    }

    std::cout << "[DDCI] Agent initialized. Model: " << config_.model_name
              << " | L1 token budget: " << config_.max_l1_tokens << "\n";
    return core::Result<void>::success();
}

core::Result<void> Agent::run_repl() {
    is_running_ = true;
    std::cout << "[DDCI] Interactive session started. "
                 "Type 'exit' or 'quit' to end.\n\n";

    while (is_running_) {
        std::cout << "DDCI > " << std::flush;
        std::string user_input;
        if (!std::getline(std::cin, user_input)) {
            std::cout << "\n[DDCI] Session ended by EOF.\n";
            is_running_ = false;
            break;
        }

        size_t start = user_input.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) {
            continue;
        }
        size_t end = user_input.find_last_not_of(" \t\r\n");
        user_input = user_input.substr(start, end - start + 1);

        if (user_input == "exit" || user_input == "quit") {
            std::cout << "[DDCI] Shutting down. Goodbye!\n";
            is_running_ = false;
            break;
        }

        auto add_user = memory_.add_message(core::Role::User, user_input, 1.0);
        if (!add_user.ok()) {
            std::cerr << "[ENGINE] Failed to store user message: "
                      << static_cast<int>(add_user.error()) << "\n";
            continue;
        }

        std::vector<core::Message> context = memory_.get_context();

        auto response = network_.chat_completion(context);
        if (!response.ok()) {
            std::cerr << "[ENGINE] Chat completion failed: "
                      << static_cast<int>(response.error()) << "\n";
            continue;
        }

        std::string reply = std::move(response).value();

        std::cout << "\nDDCI Assistant: " << reply << "\n\n";

        auto add_assistant =
            memory_.add_message(core::Role::Assistant, reply, 1.0);
        if (!add_assistant.ok()) {
            std::cerr << "[ENGINE] Failed to store assistant reply: "
                      << static_cast<int>(add_assistant.error()) << "\n";
        }
    }

    return core::Result<void>::success();
}

}
