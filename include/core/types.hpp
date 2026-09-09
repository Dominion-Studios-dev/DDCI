#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ddci::core {

enum class Role : uint8_t {
    System,
    User,
    Assistant,
    Tool
};

struct Message {
    Role role;
    std::string content;
    size_t token_count{0};
    double importance_score{0.0};
    std::chrono::system_clock::time_point timestamp{std::chrono::system_clock::now()};
};

struct MemoryEntry {
    int64_t id{-1};
    std::string category;
    std::string content;
    double importance_score{0.0};
};

}