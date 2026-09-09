#pragma once

#include <string>
#include <string_view>

namespace ddci::security {

[[nodiscard]] std::string sanitize_terminal_output(std::string_view input);

void secure_erase(std::string& str);

}