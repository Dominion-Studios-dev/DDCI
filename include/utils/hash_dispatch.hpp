#pragma once

#include <cstdint>
#include <string_view>

namespace ddci::utils {

constexpr std::uint64_t fnv1a_hash(std::string_view str) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char ch : str) {
        hash ^= static_cast<std::uint64_t>(
            static_cast<unsigned char>(ch));
        hash *= 1099511628211ULL;
    }
    return hash;
}

}