#include "core/config.hpp"
#include "core/vibe.hpp"
#include "ddci/mem/arena.hpp"
#include "ddci/net/endpoint_health.hpp"
#include "ddci/net/timing_log.hpp"
#include "engine/got_engine.hpp"
#include "governor/rate_governor.hpp"
#include "memory/context_manager.hpp"
#include "network/api_client.hpp"
#include "security/sanitizer.hpp"
#include "storage/db_client.hpp"
#include "telemetry/usage_tracker.hpp"
#include "ui/ui_render.hpp"
#include "utils/hash_dispatch.hpp"
#include "workspace/workspace_context.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view kVersion = "1.1.4";

constexpr int kHistoryLimit = 20;

constexpr std::string_view kHelpText =
    "Available commands:\n"
    "  exit / quit / /q  End the session.\n"
    "  /help             Show this help.\n"
    "  /clear            Reset the conversation and wipe stored chat history.\n"
    "  /history          Show the stored chat history.\n"
    "  /clear-history    Wipe only the stored chat history.\n"
    "  /name             Show the stored user name.\n"
    "  /name <name>      Persist or update your stored user name.\n"
    "  /got <prompt>     Run a Graph-of-Thought evaluation: N concurrent\n"
    "                    reasoning branches are scored and pruned, then\n"
    "                    aggregated into a single streamed answer.\n"
    "  anything else     Sent to the model.\n";

constexpr std::string_view kUsageText =
    "Usage: ddci [options]\n"
    "\n"
    "Options:\n"
    "  -h, --help           Show this help and exit.\n"
    "  -v, --version        Print the version and exit.\n"
    "  -m, --model <name>   Override the model (else MODEL_NAME / default).\n"
    "  -c, --config <path>  Use <path> instead of the default .env file.\n"
    "  --set-name <name>    Persist a user name (e.g. --set-name \"Vex\") and\n"
    "                       exit.\n"
    "  --get-name           Print the stored user name and exit.\n"
    "  --splash             Print the DDCI splash art and exit.\n"
    "\n"
    "Keys are discovered automatically from the environment or a local .env\n"
    "file: GROQ_API_KEY first, OPENAI_API_KEY as a fallback. On first run\n"
    "without a key, ddci walks you through creating one.\n";

std::string trim_ws(std::string s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool write_env_file(const char* env_path, const std::string& key) noexcept {
    constexpr mode_t kEnvMode = S_IRUSR | S_IWUSR;
    const int fd = ::open(env_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                          kEnvMode);
    if (fd < 0) {
        return false;
    }
    (void)::fchmod(fd, kEnvMode);

    const std::string line = "GROQ_API_KEY=" + key + "\n";
    std::size_t off = 0;
    while (off < line.size()) {
        const ssize_t w = ::write(fd, line.data() + off, line.size() - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)::close(fd);
            return false;
        }
        off += static_cast<std::size_t>(w);
    }
    (void)::fsync(fd);
    (void)::close(fd);
    return true;
}

void warn_env_perms(const char* env_path) {
    struct stat st {};
    if (::stat(env_path, &st) != 0) {
        return;
    }
    constexpr mode_t kBadBits = S_IRWXG | S_IRWXO;
    if ((st.st_mode & kBadBits) != 0) {
        std::cerr << "[WARNING] " << env_path
                  << " is group/world readable. Run: chmod 600 " << env_path
                  << "\n";
    }
}

}

int main(int argc, char** argv) {
    ::umask(0077);

    const char* env_path = ".env";
    const char* model_override = nullptr;
    const char* name_override = nullptr;
    bool get_name_only = false;
    bool splash_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            std::cout << kUsageText;
            return 0;
        }
        if (arg == "-v" || arg == "--version") {
            std::cout << "DDCI v" << kVersion << "\n";
            return 0;
        }
        if (arg == "-m" || arg == "--model") {
            if (i + 1 >= argc) {
                std::cerr << "[ERROR] Option '" << arg
                          << "' requires a model name.\n\n" << kUsageText;
                return 2;
            }
            model_override = argv[++i];
        } else if (arg.rfind("--model=", 0) == 0) {
            model_override = argv[i] + 8;
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "[ERROR] Option '" << arg
                          << "' requires a file path.\n\n" << kUsageText;
                return 2;
            }
            env_path = argv[++i];
        } else if (arg.rfind("--config=", 0) == 0) {
            env_path = argv[i] + 9;
        } else if (arg == "--set-name") {
            if (i + 1 >= argc) {
                std::cerr << "[ERROR] Option '" << arg
                          << "' requires a name.\n\n" << kUsageText;
                return 2;
            }
            name_override = argv[++i];
        } else if (arg.rfind("--set-name=", 0) == 0) {
            name_override = argv[i] + 11;
        } else if (arg == "--get-name") {
            get_name_only = true;
        } else if (arg == "--splash") {
            splash_only = true;
        } else {
            std::cerr << "[ERROR] Unknown option: " << arg << "\n\n"
                      << kUsageText;
            return 2;
        }
    }

    if (splash_only) {
        ddci::ui::print_splash(kVersion);
        return 0;
    }

    auto config_result = ddci::core::Config::load_from_env(env_path);
    if (!config_result.ok()) {
        std::cerr << "[FATAL] Failed to load configuration: "
                  << static_cast<int>(config_result.error()) << "\n";
        return 1;
    }

    auto config = std::move(config_result).value();
    if (model_override != nullptr) {
        config.model_name = model_override;
    }

    const bool interactive = ::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO);

    if (name_override != nullptr) {
        std::string name = trim_ws(name_override);
        if (name.empty()) {
            std::cerr << "[ERROR] Name cannot be empty.\n\n" << kUsageText;
            return 2;
        }
        ddci::storage::DatabaseClient db(config.db_path);
        if (!db.is_open()) {
            std::cerr << "[ERROR] Cannot open the database at "
                      << config.db_path << ".\n";
            return 1;
        }
        auto schema = db.init_schema();
        if (!schema.ok()) {
            std::cerr << "[ERROR] Database schema init failed.\n";
            return 1;
        }
        auto saved = db.set_user_name(name);
        if (!saved.ok()) {
            std::cerr << "[ERROR] Could not persist the name.\n";
            return 1;
        }
        std::cout << "[DDCI] Name updated to \"" << name
                  << "\". Locked in.\n";
        return 0;
    }

    if (get_name_only) {
        ddci::storage::DatabaseClient db(config.db_path);
        if (!db.is_open()) {
            std::cerr << "[ERROR] Cannot open the database at "
                      << config.db_path << ".\n";
            return 1;
        }
        auto schema = db.init_schema();
        if (!schema.ok()) {
            std::cerr << "[ERROR] Database schema init failed.\n";
            return 1;
        }
        auto stored = db.get_user_name();
        if (stored.ok() && !stored.value().empty()) {
            std::cout << "[DDCI] Stored name: " << stored.value() << ".\n";
        } else {
            std::cout << "[DDCI] No name stored yet.\n";
        }
        return 0;
    }

    warn_env_perms(env_path);

    if (config.groq_api_key.empty()) {
        if (interactive) {
            std::cout << "First run: no API key detected.\n"
                      << "  Create one at: https://console.groq.com/keys\n"
                      << "Enter your Groq API key: " << std::flush;
            std::string key;
            if (!std::getline(std::cin, key)) {
                std::cerr << "\n[ERROR] No API key entered. Exiting.\n";
                return 1;
            }
            key = trim_ws(std::move(key));
            if (key.empty()) {
                std::cerr << "[ERROR] No API key entered. Exiting.\n";
                return 1;
            }
            if (!write_env_file(env_path, key)) {
                std::cerr << "[ERROR] Could not write the key to " << env_path
                          << ". Check directory permissions.\n";
                return 1;
            }
            config.groq_api_key = std::move(key);
            config.api_key_source = "GROQ_API_KEY";

            std::cout << "\nSaved to " << env_path << " (0600, owner only)."
                      << "\n\n";
        } else {
            std::cerr << "[ERROR] No API key found.\n"
                      << "  Set GROQ_API_KEY (or OPENAI_API_KEY) in your "
                         "environment,\n"
                      << "  or create " << env_path
                      << " with GROQ_API_KEY=<your key>.\n";
            return 1;
        }
    }

    std::string user_name;
    ddci::storage::DatabaseClient identity_db(config.db_path);
    if (!identity_db.is_open()) {
        std::cerr << "[DDCI] Warning: identity DB unavailable at "
                  << config.db_path << " — running anonymous.\n";
    } else {
        auto schema = identity_db.init_schema();
        if (!schema.ok()) {
            std::cerr << "[DDCI] Warning: identity schema init failed — "
                         "running anonymous.\n";
        } else {
            auto stored = identity_db.get_user_name();
            if (stored.ok()) {
                user_name = std::move(stored).value();
            }

            if (user_name.empty() && interactive) {
                    std::cout << "[DDCI] Yo, before we lock in—what name "
                                 "should I save to the DB? >  "
                              << std::flush;
                    std::string entered;
                    if (std::getline(std::cin, entered)) {
                        entered = trim_ws(std::move(entered));
                        if (!entered.empty()) {
                            auto saved = identity_db.set_user_name(entered);
                            if (saved.ok()) {
                                user_name = std::move(entered);
                                std::cout << "[DDCI] Locked in. Welcome, "
                                          << user_name << ".\n";
                            } else {
                                std::cerr << "[DDCI] Could not persist your "
                                             "name — running anonymous.\n";
                            }
                        } else {
                            std::cout << "[DDCI] No name, got it — staying "
                                         "anonymous.\n";
                        }
                    } else {
                        std::cout << "\n";
                    }
                }
}
    }

    std::string repl_prompt = std::string(ddci::ui::color::kCyan) + "User"
                              + std::string(ddci::ui::color::kReset) + " > ";
    if (!user_name.empty()) {
        repl_prompt = std::string(ddci::ui::color::kCyan) + user_name
                      + std::string(ddci::ui::color::kReset) + " > ";
    }

    ddci::governor::RateGovernor governor;
    ddci::telemetry::UsageTracker tracker(config.model_name);
    ddci::network::APIClient api_client(config);

    ddci::core::VibeCache vibe;
    if (!vibe.load().ok()) {
        std::cerr << "[DDCI] Warning: system vibe unavailable — "
                     "continuing without vibe injection.\n";
    }

    ddci::net::TimingLog timing_log("timing.csv");
    ddci::net::EndpointHealth health(config.groq_api_url);

    api_client.set_usage_tracker(&tracker);
    api_client.set_timing_log(&timing_log);
    api_client.set_endpoint_health(&health);
    api_client.set_system_vibe(vibe.text());

    ddci::engine::GoTEngine got(config, api_client);
    got.set_usage_tracker(&tracker);
    got.set_timing_log(&timing_log);
    got.set_endpoint_health(&health);
    got.set_system_vibe(vibe.text());

    health.set_auth_bearer(config.groq_api_key);
    health.start();

    ddci::memory::ContextManager ctx(config);
    auto ctx_init = ctx.init();
    if (!ctx_init.ok()) {
        std::cerr << "[ERROR] Failed to initialize context manager: "
                  << static_cast<int>(ctx_init.error()) << "\n";
        return 1;
    }

    if (!user_name.empty()) {
        std::string identity_prompt =
            "The user's name is " + user_name +
            ". Always address them by name.";
        auto add_identity = ctx.add_message(ddci::core::Role::System, identity_prompt);
        if (!add_identity.ok()) {
            std::cerr << "[MEMORY] Failed to inject user identity: "
                      << static_cast<int>(add_identity.error()) << "\n";
        }
    }

    auto history = identity_db.get_recent_history(kHistoryLimit);
    if (identity_db.is_open() && history.ok() &&
        !history.value().empty()) {
        for (const auto& msg : history.value()) {
            const ddci::core::Role role =
                (msg.role == "assistant")
                    ? ddci::core::Role::Assistant
                    : ddci::core::Role::User;
            auto add_hist = ctx.add_message(role, msg.content);
            if (!add_hist.ok()) {
                std::cerr << "[MEMORY] Failed to restore chat history: "
                          << static_cast<int>(add_hist.error()) << "\n";
                break;
            }
        }
    }

    ddci::workspace::GitState git_state = ddci::workspace::detect_git_state();
    if (git_state.present) {
        std::string git_prompt = "Workspace telemetry (git): branch '"
                                 + std::string(git_state.branch) + "', "
                                 + std::to_string(git_state.modified)
                                 + " modified, " + std::to_string(git_state.staged)
                                 + " staged, " + std::to_string(git_state.untracked)
                                 + " untracked";
        if (!git_state.detail.empty()) {
            git_prompt += ". Changed files: " + git_state.detail + ".";
        }
        auto add_git = ctx.add_message(ddci::core::Role::System, git_prompt);
        if (!add_git.ok()) {
            std::cerr << "[MEMORY] Failed to inject workspace telemetry: "
                      << static_cast<int>(add_git.error()) << "\n";
        }
    }

    std::cout << ddci::ui::color::kGreen
              << "Secure chat locked in. Type 'exit', 'quit', or '/q' to end "
                 "('/help' for info)."
              << ddci::ui::color::kReset << "\n\n";

    while (true) {
        ddci::mem::Turn turn;

        std::cout << repl_prompt << std::flush;

        std::string input;
        if (!std::getline(std::cin, input)) {
            std::cout << "\n";
            break;
        }

        if (input.empty()) {
            continue;
        }

        bool shutdown = false;
        if (input == "/name" ||
            (input.size() > 6 && input.rfind("/name ", 0) == 0)) {
            if (input == "/name") {
                if (!identity_db.is_open()) {
                    std::cerr << "[DDCI] Identity database unavailable."
                              << "\n\n";
                    continue;
                }
                auto stored = identity_db.get_user_name();
                if (!stored.ok() || stored.value().empty()) {
                    std::cout << "[DDCI] No name stored yet. "
                                 "Use /name <your name>.\n\n";
                } else {
                    std::cout << "[DDCI] Stored name: "
                              << stored.value() << ".\n\n";
                }
                continue;
            }
            std::string new_name = trim_ws(std::string(input.substr(6)));
            if (new_name.empty()) {
                std::cout << "[DDCI] usage: /name <your name>\n\n";
                continue;
            }
            if (!identity_db.is_open()) {
                std::cerr << "[DDCI] Identity database unavailable — "
                             "cannot persist the name.\n\n";
                continue;
            }
            auto saved = identity_db.set_user_name(new_name);
            if (!saved.ok()) {
                std::cerr << "[DDCI] Could not persist the name to "
                             "the DB.\n\n";
                continue;
            }
            user_name = new_name;
            repl_prompt = std::string(ddci::ui::color::kCyan) + user_name
                          + std::string(ddci::ui::color::kReset) + " > ";
            std::cout << "[DDCI] Name updated to \"" << new_name
                      << "\". Locked in.\n\n";
            std::string identity_refresh =
                "Note: the user's name changed to " + new_name +
                ". Always address them by name.";
            auto add_id = ctx.add_message(ddci::core::Role::System,
                                          identity_refresh);
            if (!add_id.ok()) {
                std::cerr << "[MEMORY] Failed to refresh identity: "
                          << static_cast<int>(add_id.error()) << "\n\n";
            }
            continue;
        }

        switch (ddci::utils::fnv1a_hash(input)) {
            case ddci::utils::fnv1a_hash("exit"):
                if (input == "exit") { shutdown = true; }
                break;
            case ddci::utils::fnv1a_hash("quit"):
                if (input == "quit") { shutdown = true; }
                break;
            case ddci::utils::fnv1a_hash("/q"):
                if (input == "/q") { shutdown = true; }
                break;
            case ddci::utils::fnv1a_hash("/help"): {
                if (input == "/help") {
                    std::vector<std::string> help_lines;
                    std::size_t pos = 0;
                    while (pos < kHelpText.size()) {
                        const auto eol = kHelpText.find('\n', pos);
                        help_lines.push_back(std::string(
                            kHelpText.substr(pos, (eol == std::string_view::npos)
                                                     ? kHelpText.size() - pos
                                                     : eol - pos)));
                        pos = (eol == std::string_view::npos)
                                  ? kHelpText.size()
                                  : eol + 1;
                    }
                    ddci::ui::print_box(help_lines);
                    std::cout << "\n";
                    continue;
                }
                break;
            }
            case ddci::utils::fnv1a_hash("/clear"): {
                if (input == "/clear") {
                    auto cleared = ctx.clear();
                    auto wiped = identity_db.clear_chat_history();
                    if (cleared.ok() && wiped.ok()) {
                        std::cout << "[MEMORY] conversation + chat history "
                                     "cleared — window reset to system prompt\n\n";
                    } else {
                        std::cout << "[MEMORY] clear failed\n\n";
                    }
                    continue;
                }
                break;
            }
            case ddci::utils::fnv1a_hash("/history"): {
                if (input == "/history") {
                    if (!identity_db.is_open()) {
                        std::cerr << "[DDCI] Identity database unavailable."
                                  << "\n\n";
                        continue;
                    }
                    auto hist = identity_db.get_recent_history(kHistoryLimit);
                    if (!hist.ok()) {
                        std::cerr << "[DDCI] Could not read chat history.\n\n";
                        continue;
                    }
                    const auto& msgs = hist.value();
                    if (msgs.empty()) {
                        std::cout << "[DDCI] No chat history yet.\n\n";
                        continue;
                    }
                    std::cout << "--- Chat History (last " << msgs.size()
                              << ") ---\n";
                    for (const auto& m : msgs) {
                        std::cout << "[" << m.role << "] " << m.content
                                  << "\n";
                    }
                    std::cout << "\n";
                    continue;
                }
                break;
            }
            case ddci::utils::fnv1a_hash("/clear-history"): {
                if (input == "/clear-history") {
                    auto cleared = identity_db.clear_chat_history();
                    if (!identity_db.is_open() || !cleared.ok()) {
                        std::cerr << "[DDCI] Failed to clear chat history.\n\n";
                    } else {
                        std::cout << "[DDCI] Chat history cleared.\n\n";
                    }
                    continue;
                }
                break;
            }
            default:
                break;
        }
        if (shutdown) {
            std::cout << "Goodbye!\n";
            break;
        }

        if (input == "/got" ||
            (input.size() > 5 && input.rfind("/got ", 0) == 0)) {
            if (input == "/got") {
                std::cout << "[GOT] usage: /got <prompt>\n\n";
                continue;
            }
            constexpr std::size_t kMaxGotChars = 4000;
            std::string_view got_prompt =
                std::string_view(input).substr(5);
            if (got_prompt.size() > kMaxGotChars) {
                got_prompt = got_prompt.substr(0, kMaxGotChars);
            }
            if (got_prompt.empty()) {
                std::cout << "[GOT] usage: /got <prompt>\n\n";
                continue;
            }

            const std::uint32_t est =
                static_cast<std::uint32_t>(((got_prompt.size() + 3) / 4) *
                                           (got.options().num_branches + 1));
            if (!governor.is_allowed(est)) {
                const auto wait = governor.wait_time_ms();
                std::cerr << "[RATE LIMIT] GoT request denied — "
                          << "RPM or TPM limit reached. Retry in "
                          << wait << "ms.\n\n";
                continue;
            }

            auto got_result = got.run(got_prompt);
            governor.record(est);

            if (!got_result.ok()) {
                std::cout << "\n";
                std::cerr << "[ERROR] GoT evaluation failed with error code "
                          << static_cast<int>(got_result.error())
                          << ".\n\n";
                continue;
            }

            auto add_got = ctx.add_message(ddci::core::Role::Assistant,
                                           got_result.value());
            if (!add_got.ok()) {
                std::cerr << "[MEMORY] Failed to store GoT answer: "
                          << static_cast<int>(add_got.error()) << "\n\n";
            } else if (!identity_db.log_message("assistant",
                                                got_result.value()).ok()) {
                std::cerr << "[MEMORY] Failed to persist GoT answer to "
                             "chat history.\n\n";
            }
            continue;
        }

        const std::uint32_t estimated =
            static_cast<std::uint32_t>((input.size() + 3) / 4);

        if (!governor.is_allowed(estimated)) {
            const auto wait = governor.wait_time_ms();
            std::cerr << "[RATE LIMIT] Request denied — "
                      << "RPM or TPM limit reached. Retry in "
                      << wait << "ms.\n\n";
            continue;
        }

        const size_t turns_before = ctx.message_count();
        auto add_result =
            ctx.add_message(ddci::core::Role::User, input);
        if (!add_result.ok()) {
            std::cerr << "[MEMORY] Failed to add user turn: "
                      << static_cast<int>(add_result.error()) << "\n\n";
            continue;
        }
        if (!identity_db.log_message("user", input).ok()) {
            std::cerr << "[MEMORY] Failed to persist user turn to "
                         "chat history.\n\n";
        }

        if (ctx.message_count() < turns_before) {
            std::cout << "[MEMORY] sliding window shifted: "
                      << (turns_before - ctx.message_count())
                      << " turns offloaded to L3 (active "
                      << ctx.active_token_count() << "/"
                      << config.max_l1_tokens << " tokens)\n";
        }

        auto context = ctx.get_context();
        std::cout << ddci::ui::color::kGreen << "ddci > "
                  << ddci::ui::color::kReset << std::flush;
        auto result = api_client.chat_completion_stream(context);
        if (!result.ok()) {
            std::cout << "\n";
            continue;
        }

        governor.record(estimated);

        std::cout << "\n\n";

auto add_reply = ctx.add_message(ddci::core::Role::Assistant,
                                          result.value());
        if (!add_reply.ok()) {
            std::cerr << "[MEMORY] Failed to store assistant turn: "
                      << static_cast<int>(add_reply.error()) << "\n\n";
        } else if (!identity_db.log_message("assistant",
                                            result.value()).ok()) {
            std::cerr << "[MEMORY] Failed to persist assistant turn to "
                         "chat history.\n\n";
        }

        if (!turn.spill_free()) {
            std::cout << "[ARENA] turn budget exceeded: "
                      << turn.pad().spill_count() << " spill(s), "
                      << turn.pad().spill_bytes()
                      << " bytes (2 MiB scratchpad)\n";
        }
    }

    health.stop();
    auto flush_result = ctx.flush();
    if (flush_result.ok()) {
        std::cout << "[MEMORY] " << ctx.evicted_chunk_count()
                  << " eviction chunks persisted to L3\n";
    } else {
        std::cerr << "[MEMORY] L3 flush incomplete: "
                  << static_cast<int>(flush_result.error()) << "\n";
    }

    std::cout << "\n" << tracker.summary() << "\n";
    std::cout << "[TIMING] per-request latency ledger written to timing.csv\n";

    ddci::security::secure_erase(config.groq_api_key);
    std::cout << "[SECURE] API key erased from memory.\n";

    return 0;
}
