#include "core/config.hpp"

#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>

namespace ddci::core {

namespace {

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::pair<std::string, std::string> parse_env_line(const std::string& line) {
    std::string trimmed = trim(line);

    if (trimmed.empty() || trimmed[0] == '#') {
        return {};
    }

    const std::string export_prefix = "export ";
    if (trimmed.rfind(export_prefix, 0) == 0) {
        trimmed = trim(trimmed.substr(export_prefix.size()));
    }

    auto eq_pos = trimmed.find('=');
    if (eq_pos == std::string::npos) {
        return {};
    }

    std::string key = trim(trimmed.substr(0, eq_pos));
    std::string value = trim(trimmed.substr(eq_pos + 1));

    if (value.size() >= 2) {
        if ((value.front() == '"' && value.back() == '"') ||
            (value.front() == '\'' && value.back() == '\'')) {
            value = value.substr(1, value.size() - 2);
        }
    }

    return {std::move(key), std::move(value)};
}

std::string get_env_var(const char* name) {
    const char* val = std::getenv(name);
    return val ? std::string(val) : std::string();
}

std::pair<std::string, std::string> discover_api_key() {
    const std::string groq = get_env_var("GROQ_API_KEY");
    if (!groq.empty()) {
        return {"GROQ_API_KEY", std::move(groq)};
    }
    const std::string openai = get_env_var("OPENAI_API_KEY");
    if (!openai.empty()) {
        return {"OPENAI_API_KEY", std::move(openai)};
    }
    return {};
}

size_t parse_size_t(const std::string& s, size_t default_val) {
    if (s.empty()) return default_val;
    try {
        return static_cast<size_t>(std::stoull(s));
    } catch (...) {
        return default_val;
    }
}

}

Result<Config> Config::load_from_env(const std::string& env_path) {
    Config cfg;

    auto [key_var, key_value] = discover_api_key();
    cfg.groq_api_key = std::move(key_value);
    cfg.api_key_source = std::move(key_var);

    std::string env_groq_url = get_env_var("GROQ_API_URL");
    if (!env_groq_url.empty()) cfg.groq_api_url = env_groq_url;

    std::string env_model = get_env_var("MODEL_NAME");
    if (!env_model.empty()) cfg.model_name = env_model;

    std::string env_max_tokens = get_env_var("MAX_L1_TOKENS");
    if (!env_max_tokens.empty()) {
        cfg.max_l1_tokens = parse_size_t(env_max_tokens, cfg.max_l1_tokens);
    }

    std::string env_db_path = get_env_var("DB_PATH");
    if (!env_db_path.empty()) cfg.db_path = env_db_path;

    std::ifstream file(env_path);
    if (!file.is_open()) {
        return Result<Config>::success(std::move(cfg));
    }

    std::string line;
    while (std::getline(file, line)) {
        auto [key, value] = parse_env_line(line);
        if (key.empty() || value.empty()) continue;

        if (key == "GROQ_API_KEY" && cfg.groq_api_key.empty()) {
            cfg.groq_api_key = std::move(value);
            cfg.api_key_source = "GROQ_API_KEY";
        } else if (key == "OPENAI_API_KEY" && cfg.groq_api_key.empty()) {
            cfg.groq_api_key = std::move(value);
            cfg.api_key_source = "OPENAI_API_KEY";
        } else if (key == "GROQ_API_URL" && env_groq_url.empty()) {
            cfg.groq_api_url = std::move(value);
        } else if (key == "MODEL_NAME" && env_model.empty()) {
            cfg.model_name = std::move(value);
        } else if (key == "MAX_L1_TOKENS" && env_max_tokens.empty()) {
            cfg.max_l1_tokens = parse_size_t(value, cfg.max_l1_tokens);
        } else if (key == "DB_PATH" && env_db_path.empty()) {
            cfg.db_path = std::move(value);
        }
    }

    return Result<Config>::success(std::move(cfg));
}

}