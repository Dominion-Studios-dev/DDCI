#include "security/sanitizer.hpp"

#include <immintrin.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace ddci::security {
namespace {

inline std::size_t consume_escape(const char* data,
                                  std::size_t n,
                                  std::size_t i) {
    if (i + 1 >= n) {
        return n;
    }

    const char next = data[i + 1];

    if (next == '[') {
        i += 2;
        while (i < n) {
            const unsigned char c = static_cast<unsigned char>(data[i]);
            if (c >= 0x40 && c <= 0x7E) {
                ++i;
                break;
            }
            ++i;
        }
        return i;
    }

    if (next == ']' || next == '_' || next == 'P') {
        i += 2;
        while (i < n) {
            const unsigned char c = static_cast<unsigned char>(data[i]);
            if (c == 0x07) {
                ++i;
                break;
            }
            if (c == 0x1B && i + 1 < n && data[i + 1] == '\\') {
                i += 2;
                break;
            }
            ++i;
        }
        return i;
    }

    return i + 1;
}

}

std::string sanitize_terminal_output(const std::string_view input) {
    if (input.empty()) [[unlikely]] {
        return std::string{};
    }

    const char* data = input.data();
    const std::size_t n = input.size();

    std::string safe;
    safe.reserve(n);

    std::size_t i = 0;

#if defined(__AVX2__)
    const __m256i kTab   = _mm256_set1_epi8(0x09);
    const __m256i kNl    = _mm256_set1_epi8(0x0A);
    const __m256i kHigh  = _mm256_set1_epi8(0xE0);
    const __m256i kDel   = _mm256_set1_epi8(0x7F);
    const __m256i kZero  = _mm256_setzero_si256();

    while (i + 32 <= n) {
        const __m256i chunk =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));

        const __m256i low_bits =
            _mm256_cmpeq_epi8(_mm256_and_si256(chunk, kHigh), kZero);

        const __m256i allowed = _mm256_or_si256(
            _mm256_cmpeq_epi8(chunk, kTab), _mm256_cmpeq_epi8(chunk, kNl));

        const __m256i dirty_ctrl = _mm256_andnot_si256(allowed, low_bits);

        const __m256i dirty_del = _mm256_cmpeq_epi8(chunk, kDel);

        const int mask = _mm256_movemask_epi8(
            _mm256_or_si256(dirty_ctrl, dirty_del));

        if (mask == 0) [[likely]] {
            safe.append(data + i, 32);
            i += 32;
            continue;
        }

        const int first = __builtin_ctz(static_cast<unsigned>(mask));
        safe.append(data + i, static_cast<std::size_t>(first));
        i += static_cast<std::size_t>(first);

        if (data[i] == static_cast<char>(0x1B)) [[unlikely]] {
            i = consume_escape(data, n, i);
        } else {
            ++i;
        }
    }
#endif

    while (i < n) {
        const unsigned char ch = static_cast<unsigned char>(data[i]);

        if (ch == 0x1B) [[unlikely]] {
            i = consume_escape(data, n, i);
            continue;
        }
        if (((ch < 0x20 && ch != 0x0A && ch != 0x09) || ch == 0x7F)) [[unlikely]] {
            ++i;
            continue;
        }

        safe.push_back(data[i]);
        ++i;
    }

    return safe;
}

void secure_erase(std::string& str) {
    if (str.empty()) {
        return;
    }

    volatile char* ptr = reinterpret_cast<volatile char*>(str.data());
    const std::size_t len = str.size();
    for (std::size_t i = 0; i < len; ++i) {
        ptr[i] = 0;
    }

    str.clear();
}

}