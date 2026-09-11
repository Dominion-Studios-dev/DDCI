#include "ui/ui_render.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>

namespace ddci::ui {

namespace {

constexpr std::string_view kSplashRows[] = {
    "                    ▲",
    "              ▲       ▄▄▄▄▄▄▄▄▄▄▄▄       ▲",
    "            ▲       ▄████████████████▄",
    "          ▲       ▄████████████████████▄",
    "        ▲       ▄████████████████████████▄",
    "              ██▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓██",
    "◄             ██▓▓░░░░░░░░░░░░░░░░░░░░░░░░▓▓██             ►",
    "              ██▓▓▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▓▓██",
    "◄             ██▓▓ ▄▄▄▄▄▄  ░░  ▄▄▄▄▄▄ ▓▓██             ►",
    "              ██▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓██",
    "              ██▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓██",
    "              ▀██████████████████████████▀",
    "◄             ████████▄▄▄▄     ▄▄▄▄████████             ►",
    "             █████████████▌▐▄▄ ▄▄▐▌█████████████",
    "             █████████████▌▐████▐▌█████████████",
    "             █████████████████████████████████",
    "◄             ████████▄▄▄▓█▄▄▄▓█▓█████▓█▄▄▄▓█▄▄▄▓██             ►",
    "             ████████╳▓▓▓█▄▄▄▓█▓█████▓█▄▄▄▓█╳▓▓██",
    "             █████████████████████████████████",
    "              ▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀",
    "                   ▲          ▲          ▲",
    "                    ▲          ▲",
};

constexpr std::string_view kHeadRows[] = {
    "        ▲                     ▲",
    "      ▲       ▄▄▄▄▄▄▄▄▄▄      ▲",
    "    ▲      ▄████████████▄",
    "       ▄████████████████▄",
    "       ██▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓██",
    "       ██▓▓░░░░░░░░░░░░░░▓▓██",
    "◄      ██▓▓ ▄▄▄▄▄▄ ░░ ▄▄▄▄▄▄ ▓▓██      ►",
    "       ██▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓██",
    "       ▀███████████████████▀",
};

std::string gradient_code(int g, int b) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "\x1b[38;2;0;%d;%dm", g, b);
    return std::string(buf);
}

void print_rows(const std::string_view* rows, std::size_t count,
                std::string_view base_color) {
    for (std::size_t i = 0; i < count; ++i) {
        std::string_view row = rows[i];
        while (!row.empty() && row.back() == ' ') {
            row.remove_suffix(1);
        }
        std::cout << base_color << row << color::kReset << "\n";
    }
}

std::string repeat(std::string_view s, std::size_t n) {
    std::string out;
    out.reserve(s.size() * n);
    for (std::size_t i = 0; i < n; ++i) {
        out.append(s.data(), s.size());
    }
    return out;
}

}

std::size_t display_width(std::string_view text) noexcept {
    std::size_t width = 0;
    bool esc = false;
    for (const unsigned char c : text) {
        if (esc) {
            if (c == 'm') {
                esc = false;
            }
            continue;
        }
        if (c == '\x1b') {
            esc = true;
            continue;
        }
        if ((c & 0xC0) == 0x80) {
            continue;
        }
        ++width;
    }
    return width;
}

void print_box(const std::vector<std::string>& lines) {
    std::size_t width = 0;
    for (const auto& line : lines) {
        width = std::max(width, display_width(line));
    }
    const std::string border = repeat("\u2500", width + 2);
    std::cout << color::kCyanDim << "\u250c" << border << "\u2510"
              << color::kReset << "\n";
    for (const auto& line : lines) {
        const std::size_t pad = width - display_width(line);
        std::cout << color::kCyanDim << "\u2502 " << line
                  << std::string(pad, ' ') << " \u2502" << color::kReset
                  << "\n";
    }
    std::cout << color::kCyanDim << "\u2514" << border << "\u2518"
              << color::kReset << "\n";
}

void print_logo(std::string_view version) {
    std::cout << "\n" << color::kBold;
    print_rows(kHeadRows, sizeof(kHeadRows) / sizeof(kHeadRows[0]),
               color::kCyan);
    std::cout << color::kGreenHi << "        DDCI  v" << version
              << color::kReset << "\n\n";
}

void print_splash(std::string_view version) {
    std::cout << "\n" << color::kBold;
    const std::size_t count =
        sizeof(kSplashRows) / sizeof(kSplashRows[0]);
    for (std::size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) /
                         static_cast<double>(count - 1);
        const int g = 229 - static_cast<int>(t * 24.0);
        const int b = 255 - static_cast<int>(t * 179.0);
        std::string_view row = kSplashRows[i];
        while (!row.empty() && row.back() == ' ') {
            row.remove_suffix(1);
        }
        std::cout << gradient_code(g, b) << row << color::kReset << "\n";
    }
    std::cout << color::kGreenHi << "        DDCI  v" << version
              << color::kReset << "\n\n";
}

}