#pragma once

#include "core/error.hpp"

#include <string>
#include <string_view>

namespace ddci::core {

class VibeCache {
public:
    VibeCache() = default;

    VibeCache(const VibeCache&) = delete;
    VibeCache& operator=(const VibeCache&) = delete;

    [[nodiscard]] core::Result<void> load();

    [[nodiscard]] std::string_view text() const noexcept { return text_; }
    [[nodiscard]] bool loaded() const noexcept { return loaded_; }

private:
    std::string text_;
    bool loaded_{false};
};

}