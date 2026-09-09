#include "memory/pruner.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace ddci::memory::prune {
namespace {

std::string collapse_whitespace(const std::string_view input) {
    std::string out;
    out.reserve(input.size());

    const char* d = input.data();
    const std::size_t n = input.size();
    std::size_t i = 0;

    while (i < n && (d[i] == ' ' || d[i] == '\t' || d[i] == '\n')) {
        ++i;
    }

    int newline_run = 0;
    bool pending_space = false;

    for (; i < n; ++i) {
        const char c = d[i];

        if (c == '\n') {
            ++newline_run;
            pending_space = false;
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (newline_run == 0) {
                pending_space = true;
            }
            continue;
        }

        if (newline_run > 0) {
            out.append(newline_run >= 2 ? "\n\n" : "\n");
            newline_run = 0;
        } else if (pending_space && !out.empty() && out.back() != '\n') {
            out.push_back(' ');
        }
        pending_space = false;

        out.push_back(c);
    }

    return out;
}

void collapse_punctuation(std::string& s) {
    if (s.size() < 2) {
        return;
    }

    std::string out;
    out.reserve(s.size());

    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const char c = s[i];

        if (c == '!' || c == '?') {
            out.push_back(c);
            ++i;
            while (i < n && s[i] == c) {
                ++i;
            }
            continue;
        }

        if (c == '.') {
            std::size_t run_end = i;
            while (run_end < n && s[run_end] == '.') {
                ++run_end;
            }
            const std::size_t run = run_end - i;
            if (run == 3) {
                out.append("...");
            } else {
                out.push_back('.');
            }
            i = run_end;
            continue;
        }

        out.push_back(c);
        ++i;
    }

    s.swap(out);
}

bool is_hesitation_filler(const std::string_view token) {
    static constexpr std::string_view kFillers[] = {
        "um", "uh", "erm", "er", "hmm", "umm", "uhh", "uhm", "heh"
    };
    for (const auto f : kFillers) {
        if (token == f) {
            return true;
        }
    }
    return false;
}

std::string remove_fillers(const std::string_view s) {
    std::string out;
    out.reserve(s.size());

    const char* d = s.data();
    const std::size_t n = s.size();
    std::size_t i = 0;
    bool first = true;

    while (i < n) {
        while (i < n && (d[i] == ' ' || d[i] == '\t' || d[i] == '\n')) {
            ++i;
        }
        if (i >= n) {
            break;
        }

        const std::size_t token_begin = i;
        while (i < n && d[i] != ' ' && d[i] != '\t' && d[i] != '\n') {
            ++i;
        }
        const std::string_view token(d + token_begin, i - token_begin);

        std::size_t lo = 0;
        std::size_t hi = token.size();
        while (lo < hi &&
               !std::isalpha(static_cast<unsigned char>(token[lo]))) {
            ++lo;
        }
        while (hi > lo &&
               !std::isalpha(static_cast<unsigned char>(token[hi - 1]))) {
            --hi;
        }
        const std::string_view core = token.substr(lo, hi - lo);

        bool is_filler = false;
        if (core.size() <= 4) {
            char buf[5];
            const std::size_t len = std::min<std::size_t>(core.size(), 4);
            for (std::size_t k = 0; k < len; ++k) {
                buf[k] = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(core[k])));
            }
            is_filler = is_hesitation_filler(std::string_view(buf, len));
        }

        if (is_filler) {
            continue;
        }

        if (!first) {
            out.push_back(' ');
        }
        first = false;
        out.append(token);
    }

    return out;
}

void replace_all_ci(std::string& s,
                    const std::string_view needle,
                    const std::string_view replacement) {
    if (needle.empty() || needle.size() > s.size()) {
        return;
    }

    std::string out;
    out.reserve(s.size());

    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        bool match = (i + needle.size() <= n);
        for (std::size_t k = 0; match && k < needle.size(); ++k) {
            const unsigned char a = static_cast<unsigned char>(s[i + k]);
            const unsigned char b = static_cast<unsigned char>(needle[k]);
            if (std::tolower(a) != std::tolower(b)) {
                match = false;
            }
        }
        if (match) {
            out.append(replacement);
            i += needle.size();
        } else {
            out.push_back(s[i]);
            ++i;
        }
    }

    s.swap(out);
}

}

std::string compress_context(const std::string_view input) {
    std::string structural = collapse_whitespace(input);
    collapse_punctuation(structural);
    return remove_fillers(structural);
}

std::string compress_system_prompt(const std::string_view prompt) {
    static constexpr std::pair<std::string_view, std::string_view> kPhrases[] = {
        {"you are an artificial intelligence assistant", "ROLE:AI"},
        {"in the context of this conversation", ""},
        {"please remember that you must", "MUST"},
        {"make sure to always", "ALWAYS"},
        {"failure to do so will result in", "ELSE"},
        {"as a high-performance local agent", "AGENT:HP"},
        {"you respond concisely precisely and helpfully",
         "STYLE:CONCISE_PRECISE_HELPFUL"},
    };

    std::string out(prompt);
    for (const auto& [verbose, compact] : kPhrases) {
        replace_all_ci(out, verbose, compact);
    }

    return compress_context(out);
}

}
