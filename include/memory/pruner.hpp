#pragma once

#include <string>
#include <string_view>

namespace ddci::memory::prune {

[[nodiscard]] std::string compress_context(std::string_view input);

[[nodiscard]] std::string compress_system_prompt(std::string_view prompt);

}
