#pragma once

#include <cstddef>
#include <string_view>

namespace ddci::memory {

inline size_t estimate_tokens(std::string_view text) noexcept {
    return (text.length() + 3) / 4;
}

}
