#pragma once

#include <cstdint>
#include <string>

namespace ddci::workspace {

struct GitState {
    bool present{false};
    char branch[128]{'\0'};
    std::uint32_t staged{0};
    std::uint32_t modified{0};
    std::uint32_t untracked{0};
    std::string detail;
};

GitState detect_git_state();

}