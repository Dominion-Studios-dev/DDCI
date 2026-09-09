#include "core/config.hpp"
#include "ddci/mem/arena.hpp"
#include "ddci/net/endpoint_health.hpp"
#include "ddci/net/timing_log.hpp"
#include "engine/got_engine.hpp"
#include "governor/rate_governor.hpp"
#include "memory/context_manager.hpp"
#include "network/api_client.hpp"
#include "security/sanitizer.hpp"
#include "telemetry/usage_tracker.hpp"
#include "utils/hash_dispatch.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view kVersion = "1.0.0";

constexpr std::string_view kHelpText =
    "Available commands:\n"
    "  exit / quit / /q  End the session.\n"
    "  /help             Show this help.\n"
    "  /clear            Reset the conversation window.\n"
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
        } else {
            std::cerr << "[ERROR] Unknown option: " << arg << "\n\n"
                      << kUsageText;
            return 2;
        }
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

    std::cout << "=== DDCI Configuration ===\n";
    std::cout << "  Model:      " << config.model_name << "\n";
    std::cout << "  Endpoint:   " << config.groq_api_url << "\n";

    if (config.groq_api_key.empty()) {
        std::cout << "  API Key:    [NOT SET]\n";
    } else {
        std::cout << "  API Key:    " << config.groq_api_key.substr(0, 8)
                  << "... (length " << config.groq_api_key.size()
                  << ", via " << (config.api_key_source.empty()
                                      ? "unknown source"
                                      : config.api_key_source)
                  << ")\n";
    }

    std::cout << "  Max L1:     " << config.max_l1_tokens << " tokens\n";
    std::cout << "  DB Path:    " << config.db_path << "\n";
    std::cout << "==========================\n\n";

    warn_env_perms(env_path);

    if (config.groq_api_key.empty()) {
        const bool interactive = ::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO);
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

    ddci::governor::RateGovernor governor;
    ddci::telemetry::UsageTracker tracker(config.model_name);
    ddci::network::APIClient api_client(config);

    ddci::net::TimingLog timing_log("timing.csv");
    ddci::net::EndpointHealth health(config.groq_api_url);

    api_client.set_usage_tracker(&tracker);
    api_client.set_timing_log(&timing_log);
    api_client.set_endpoint_health(&health);

    ddci::engine::GoTEngine got(config, api_client);
    got.set_usage_tracker(&tracker);
    got.set_timing_log(&timing_log);
    got.set_endpoint_health(&health);

    health.set_auth_bearer(config.groq_api_key);
    health.start();

    ddci::memory::ContextManager ctx(config);
    auto ctx_init = ctx.init();
    if (!ctx_init.ok()) {
        std::cerr << "[ERROR] Failed to initialize context manager: "
                  << static_cast<int>(ctx_init.error()) << "\n";
        return 1;
    }

    std::cout << "  Memory:     sliding-window L1, "
              << ctx.active_token_count() << " tokens seeded, "
              << "evict @" << static_cast<int>(0.80 * 100.0) << "%, "
              << "rearm @" << static_cast<int>(0.60 * 100.0) << "%\n";

    std::cout << "=== Secure chat started. "
                 "Type 'exit', 'quit', or '/q' to end ('/help' for info). ===\n\n";

    while (true) {
        ddci::mem::Turn turn;

        std::cout << "User > " << std::flush;

        std::string input;
        if (!std::getline(std::cin, input)) {
            std::cout << "\n";
            break;
        }

        if (input.empty()) {
            continue;
        }

        bool shutdown = false;
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
                    std::cout << kHelpText << "\n";
                    continue;
                }
                break;
            }
            case ddci::utils::fnv1a_hash("/clear"): {
                if (input == "/clear") {
                    auto cleared = ctx.clear();
                    std::cout << (cleared.ok()
                        ? "[MEMORY] conversation cleared — window reset "
                          "to system prompt\n\n"
                        : "[MEMORY] clear failed\n\n");
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

        if (ctx.message_count() < turns_before) {
            std::cout << "[MEMORY] sliding window shifted: "
                      << (turns_before - ctx.message_count())
                      << " turns offloaded to L3 (active "
                      << ctx.active_token_count() << "/"
                      << config.max_l1_tokens << " tokens)\n";
        }

        auto context = ctx.get_context();
        std::cout << "Groq > " << std::flush;
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
