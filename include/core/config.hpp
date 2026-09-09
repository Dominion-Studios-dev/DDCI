#pragma once

#include "error.hpp"

#include <cstddef>
#include <string>

namespace ddci::core {

struct Config {
    std::string groq_api_key;
    std::string api_key_source;
    std::string groq_api_url{"https://api.groq.com/openai/v1/chat/completions"};
    std::string model_name{"llama-3.3-70b-versatile"};

    size_t max_l1_tokens{8192};

    std::string db_path{"ddci_memory.db"};

    static Result<Config> load_from_env(const std::string& env_path = ".env");
};

}