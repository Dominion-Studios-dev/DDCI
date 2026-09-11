#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ddci::ui {

namespace color {
inline constexpr std::string_view kReset   = "\x1b[0m";
inline constexpr std::string_view kBold    = "\x1b[1m";
inline constexpr std::string_view kGreen   = "\x1b[38;2;0;205;76m";
inline constexpr std::string_view kGreenHi = "\x1b[38;2;120;255;150m";
inline constexpr std::string_view kCyan    = "\x1b[38;2;0;229;255m";
inline constexpr std::string_view kCyanDim = "\x1b[38;2;70;130;160m";
inline constexpr std::string_view kWhite   = "\x1b[38;2;230;240;250m";
inline constexpr std::string_view kGray    = "\x1b[38;2;120;130;140m";
}

std::size_t display_width(std::string_view text) noexcept;
void print_box(const std::vector<std::string>& lines);
void print_logo(std::string_view version);
void print_splash(std::string_view version);

}