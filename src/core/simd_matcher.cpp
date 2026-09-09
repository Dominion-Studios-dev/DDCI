#include "ddci/core/simd_matcher.hpp"

#include <cmath>
#include <cstring>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define DDCI_HAS_X86_TARGET 1
#endif

#if defined(__aarch64__)
#include <arm_neon.h>
#define DDCI_HAS_NEON 1
#endif

namespace ddci::core {

namespace {

constexpr char kWsSet[] = {' ', '\t', '\n', '\v', '\f', '\r'};
constexpr std::size_t kWsSetLen = 6;

inline bool is_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' ||
           c == '\v' || c == '\f' || c == '\r';
}

inline std::size_t ctz64(std::uint64_t m) noexcept {
    return static_cast<std::size_t>(__builtin_ctzll(m));
}

std::ptrdiff_t find_byte_scalar(const char* p, std::size_t n, char c) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i] == c) {
            return static_cast<std::ptrdiff_t>(i);
        }
    }
    return static_cast<std::ptrdiff_t>(n);
}

std::size_t count_byte_scalar(const char* p, std::size_t n, char c) noexcept {
    std::size_t k = 0;
    for (std::size_t i = 0; i < n; ++i) {
        k += (p[i] == c) ? 1u : 0u;
    }
    return k;
}

std::ptrdiff_t find_first_of_scalar(const char* p, std::size_t n,
                                    const char* set,
                                    std::size_t set_len) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t s = 0; s < set_len; ++s) {
            if (p[i] == set[s]) {
                return static_cast<std::ptrdiff_t>(i);
            }
        }
    }
    return static_cast<std::ptrdiff_t>(n);
}

#if defined(DDCI_HAS_X86_TARGET)

__attribute__((target("avx512f,avx512bw")))
std::ptrdiff_t find_byte_avx512(const char* p, std::size_t n, char c) noexcept {
    const __m512i needle = _mm512_set1_epi8(c);
    std::size_t off = 0;
    for (; off + 64 <= n; off += 64) {
        const __mmask64 m = _mm512_cmpeq_epi8_mask(
            _mm512_loadu_si512(p + off), needle);
        if (m != 0) {
            return static_cast<std::ptrdiff_t>(
                off + ctz64(static_cast<std::uint64_t>(m)));
        }
    }
    if (off < n) {
        char tmp[64];
        const std::size_t tail = n - off;
        std::memcpy(tmp, p + off, tail);
        const __mmask64 m =
            _mm512_cmpeq_epi8_mask(_mm512_loadu_si512(tmp), needle) &
            ((static_cast<__mmask64>(1) << tail) - 1);
        if (m != 0) {
            return static_cast<std::ptrdiff_t>(
                off + ctz64(static_cast<std::uint64_t>(m)));
        }
    }
    return static_cast<std::ptrdiff_t>(n);
}

__attribute__((target("avx512f,avx512bw")))
std::size_t count_byte_avx512(const char* p, std::size_t n, char c) noexcept {
    const __m512i needle = _mm512_set1_epi8(c);
    std::size_t off = 0;
    std::size_t total = 0;
    for (; off + 64 <= n; off += 64) {
        const __mmask64 m = _mm512_cmpeq_epi8_mask(
            _mm512_loadu_si512(p + off), needle);
        total += __builtin_popcountll(static_cast<std::uint64_t>(m));
    }
    if (off < n) {
        char tmp[64];
        const std::size_t tail = n - off;
        std::memcpy(tmp, p + off, tail);
        const __mmask64 m =
            _mm512_cmpeq_epi8_mask(_mm512_loadu_si512(tmp), needle) &
            ((static_cast<__mmask64>(1) << tail) - 1);
        total += __builtin_popcountll(static_cast<std::uint64_t>(m));
    }
    return total;
}

__attribute__((target("avx512f,avx512bw")))
std::ptrdiff_t find_first_of_avx512(const char* p, std::size_t n,
                                    const char* set,
                                    std::size_t set_len) noexcept {
    __m512i needles[8];
    for (std::size_t s = 0; s < set_len; ++s) {
        needles[s] = _mm512_set1_epi8(set[s]);
    }
    std::size_t off = 0;
    for (; off + 64 <= n; off += 64) {
        const __m512i block = _mm512_loadu_si512(p + off);
        __mmask64 acc = 0;
        for (std::size_t s = 0; s < set_len; ++s) {
            acc |= _mm512_cmpeq_epi8_mask(block, needles[s]);
        }
        if (acc != 0) {
            return static_cast<std::ptrdiff_t>(
                off + ctz64(static_cast<std::uint64_t>(acc)));
        }
    }
    if (off < n) {
        char tmp[64];
        const std::size_t tail = n - off;
        std::memcpy(tmp, p + off, tail);
        const __m512i block = _mm512_loadu_si512(tmp);
        __mmask64 acc = 0;
        for (std::size_t s = 0; s < set_len; ++s) {
            acc |= _mm512_cmpeq_epi8_mask(block, needles[s]);
        }
        acc &= ((static_cast<__mmask64>(1) << tail) - 1);
        if (acc != 0) {
            return static_cast<std::ptrdiff_t>(
                off + ctz64(static_cast<std::uint64_t>(acc)));
        }
    }
    return static_cast<std::ptrdiff_t>(n);
}

__attribute__((target("avx512f")))
void dot_norms_avx512(const float* a, const float* b, std::size_t n,
                      float& dot, float& na, float& nb) noexcept {
    __m512 d = _mm512_setzero_ps();
    __m512 x = _mm512_setzero_ps();
    __m512 y = _mm512_setzero_ps();
    std::size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        const __m512 av = _mm512_loadu_ps(a + i);
        const __m512 bv = _mm512_loadu_ps(b + i);
        d = _mm512_fmadd_ps(av, bv, d);
        x = _mm512_fmadd_ps(av, av, x);
        y = _mm512_fmadd_ps(bv, bv, y);
    }
    float sd = _mm512_reduce_add_ps(d);
    float sx = _mm512_reduce_add_ps(x);
    float sy = _mm512_reduce_add_ps(y);
    for (; i < n; ++i) {
        sd += a[i] * b[i];
        sx += a[i] * a[i];
        sy += b[i] * b[i];
    }
    dot = sd;
    na = sx;
    nb = sy;
}

__attribute__((target("avx512f")))
float sum_sq_diff_avx512(const float* a, const float* b,
                         std::size_t n) noexcept {
    __m512 acc = _mm512_setzero_ps();
    std::size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        const __m512 d =
            _mm512_sub_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i));
        acc = _mm512_fmadd_ps(d, d, acc);
    }
    float s = _mm512_reduce_add_ps(acc);
    for (; i < n; ++i) {
        const float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}

__attribute__((target("avx2,fma")))
std::ptrdiff_t find_byte_avx2(const char* p, std::size_t n, char c) noexcept {
    const __m256i needle = _mm256_set1_epi8(c);
    std::size_t off = 0;
    for (; off + 32 <= n; off += 32) {
        const __m256i eq = _mm256_cmpeq_epi8(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + off)),
            needle);
        const std::uint32_t m =
            static_cast<std::uint32_t>(_mm256_movemask_epi8(eq));
        if (m != 0) {
            return static_cast<std::ptrdiff_t>(off + ctz64(m));
        }
    }
    if (off < n) {
        char tmp[32];
        const std::size_t tail = n - off;
        std::memcpy(tmp, p + off, tail);
        const __m256i eq = _mm256_cmpeq_epi8(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(tmp)), needle);
        const std::uint32_t m =
            static_cast<std::uint32_t>(_mm256_movemask_epi8(eq)) &
            ((std::uint32_t{1} << tail) - 1);
        if (m != 0) {
            return static_cast<std::ptrdiff_t>(off + ctz64(m));
        }
    }
    return static_cast<std::ptrdiff_t>(n);
}

__attribute__((target("avx2,fma")))
std::size_t count_byte_avx2(const char* p, std::size_t n, char c) noexcept {
    const __m256i needle = _mm256_set1_epi8(c);
    std::size_t off = 0;
    std::size_t total = 0;
    for (; off + 32 <= n; off += 32) {
        const __m256i eq = _mm256_cmpeq_epi8(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + off)),
            needle);
        total += __builtin_popcount(
            static_cast<unsigned>(_mm256_movemask_epi8(eq)));
    }
    if (off < n) {
        char tmp[32];
        const std::size_t tail = n - off;
        std::memcpy(tmp, p + off, tail);
        const __m256i eq = _mm256_cmpeq_epi8(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(tmp)), needle);
        total += __builtin_popcount(
            static_cast<unsigned>(_mm256_movemask_epi8(eq)) &
            ((std::uint32_t{1} << tail) - 1));
    }
    return total;
}

__attribute__((target("avx2,fma")))
std::ptrdiff_t find_first_of_avx2(const char* p, std::size_t n,
                                  const char* set,
                                  std::size_t set_len) noexcept {
    __m256i needles[8];
    for (std::size_t s = 0; s < set_len; ++s) {
        needles[s] = _mm256_set1_epi8(set[s]);
    }
    std::size_t off = 0;
    for (; off + 32 <= n; off += 32) {
        const __m256i block =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + off));
        std::uint32_t acc = 0;
        for (std::size_t s = 0; s < set_len; ++s) {
            acc |= static_cast<std::uint32_t>(
                _mm256_movemask_epi8(_mm256_cmpeq_epi8(block, needles[s])));
        }
        if (acc != 0) {
            return static_cast<std::ptrdiff_t>(off + ctz64(acc));
        }
    }
    if (off < n) {
        char tmp[32];
        const std::size_t tail = n - off;
        std::memcpy(tmp, p + off, tail);
        const __m256i block =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(tmp));
        std::uint32_t acc = 0;
        for (std::size_t s = 0; s < set_len; ++s) {
            acc |= static_cast<std::uint32_t>(
                _mm256_movemask_epi8(_mm256_cmpeq_epi8(block, needles[s])));
        }
        acc &= ((std::uint32_t{1} << tail) - 1);
        if (acc != 0) {
            return static_cast<std::ptrdiff_t>(off + ctz64(acc));
        }
    }
    return static_cast<std::ptrdiff_t>(n);
}

__attribute__((target("avx2,fma")))
void dot_norms_avx2(const float* a, const float* b, std::size_t n,
                    float& dot, float& na, float& nb) noexcept {
    __m256 d1 = _mm256_setzero_ps();
    __m256 d2 = _mm256_setzero_ps();
    __m256 x1 = _mm256_setzero_ps();
    __m256 x2 = _mm256_setzero_ps();
    __m256 y1 = _mm256_setzero_ps();
    __m256 y2 = _mm256_setzero_ps();
    std::size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        const __m256 a0 = _mm256_loadu_ps(a + i);
        const __m256 b0 = _mm256_loadu_ps(b + i);
        const __m256 a1 = _mm256_loadu_ps(a + i + 8);
        const __m256 b1 = _mm256_loadu_ps(b + i + 8);
        const __m256 a2 = _mm256_loadu_ps(a + i + 16);
        const __m256 b2 = _mm256_loadu_ps(b + i + 16);
        const __m256 a3 = _mm256_loadu_ps(a + i + 24);
        const __m256 b3 = _mm256_loadu_ps(b + i + 24);
        d1 = _mm256_fmadd_ps(a0, b0, d1);
        d2 = _mm256_fmadd_ps(a1, b1, d2);
        x1 = _mm256_fmadd_ps(a0, a0, x1);
        x2 = _mm256_fmadd_ps(a1, a1, x2);
        y1 = _mm256_fmadd_ps(b0, b0, y1);
        y2 = _mm256_fmadd_ps(b1, b1, y2);
        d1 = _mm256_fmadd_ps(a2, b2, d1);
        d2 = _mm256_fmadd_ps(a3, b3, d2);
        x1 = _mm256_fmadd_ps(a2, a2, x1);
        x2 = _mm256_fmadd_ps(a3, a3, x2);
        y1 = _mm256_fmadd_ps(b2, b2, y1);
        y2 = _mm256_fmadd_ps(b3, b3, y2);
    }
    const __m256 d = _mm256_add_ps(d1, d2);
    const __m256 x = _mm256_add_ps(x1, x2);
    const __m256 y = _mm256_add_ps(y1, y2);
    __m128 dl = _mm256_castps256_ps128(d);
    __m128 dh = _mm256_extractf128_ps(d, 1);
    __m128 s = _mm_add_ps(dl, dh);
    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);
    float sd = _mm_cvtss_f32(s);
    __m128 xl = _mm256_castps256_ps128(x);
    __m128 xh = _mm256_extractf128_ps(x, 1);
    __m128 xs = _mm_add_ps(xl, xh);
    xs = _mm_hadd_ps(xs, xs);
    xs = _mm_hadd_ps(xs, xs);
    float sx = _mm_cvtss_f32(xs);
    __m128 yl = _mm256_castps256_ps128(y);
    __m128 yh = _mm256_extractf128_ps(y, 1);
    __m128 ys = _mm_add_ps(yl, yh);
    ys = _mm_hadd_ps(ys, ys);
    ys = _mm_hadd_ps(ys, ys);
    float sy = _mm_cvtss_f32(ys);
    for (; i < n; ++i) {
        sd += a[i] * b[i];
        sx += a[i] * a[i];
        sy += b[i] * b[i];
    }
    dot = sd;
    na = sx;
    nb = sy;
}

__attribute__((target("avx2,fma")))
float sum_sq_diff_avx2(const float* a, const float* b,
                       std::size_t n) noexcept {
    __m256 acc1 = _mm256_setzero_ps();
    __m256 acc2 = _mm256_setzero_ps();
    std::size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        const __m256 d0 =
            _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
        const __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8),
                                        _mm256_loadu_ps(b + i + 8));
        const __m256 d2 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 16),
                                        _mm256_loadu_ps(b + i + 16));
        const __m256 d3 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 24),
                                        _mm256_loadu_ps(b + i + 24));
        acc1 = _mm256_fmadd_ps(d0, d0, acc1);
        acc2 = _mm256_fmadd_ps(d1, d1, acc2);
        acc1 = _mm256_fmadd_ps(d2, d2, acc1);
        acc2 = _mm256_fmadd_ps(d3, d3, acc2);
    }
    __m256 acc = _mm256_add_ps(acc1, acc2);
    __m128 lo = _mm256_castps256_ps128(acc);
    __m128 hi = _mm256_extractf128_ps(acc, 1);
    __m128 s = _mm_add_ps(lo, hi);
    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);
    float out = _mm_cvtss_f32(s);
    for (; i < n; ++i) {
        const float d = a[i] - b[i];
        out += d * d;
    }
    return out;
}

#endif

#if defined(DDCI_HAS_NEON)

std::ptrdiff_t find_byte_neon(const char* p, std::size_t n, char c) noexcept {
    const uint8x16_t needle = vdupq_n_u8(static_cast<uint8_t>(c));
    std::size_t off = 0;
    for (; off + 16 <= n; off += 16) {
        const uint8x16_t eq =
            vceqq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(p) + off),
                     needle);
        const uint64x2_t h = vreinterpretq_u64_u8(eq);
        const uint64_t l = vgetq_lane_u64(h, 0);
        const uint64_t r = vgetq_lane_u64(h, 1);
        if (l != 0 || r != 0) {
            for (std::size_t k = 0; k < 16; ++k) {
                if (vgetq_lane_u8(eq, k) == 0) {
                    return static_cast<std::ptrdiff_t>(off + k);
                }
            }
        }
    }
    for (; off < n; ++off) {
        if (p[off] == c) {
            return static_cast<std::ptrdiff_t>(off);
        }
    }
    return static_cast<std::ptrdiff_t>(n);
}

std::size_t count_byte_neon(const char* p, std::size_t n, char c) noexcept {
    const uint8x16_t needle = vdupq_n_u8(static_cast<uint8_t>(c));
    std::size_t off = 0;
    std::size_t total = 0;
    for (; off + 16 <= n; off += 16) {
        const uint8x16_t eq =
            vceqq_u8(vld1q_u8(reinterpret_cast<const uint8_t*>(p) + off),
                     needle);
        total += vaddvq_u8(vshrq_n_u8(eq, 7));
    }
    for (; off < n; ++off) {
        total += (p[off] == c) ? 1u : 0u;
    }
    return total;
}

std::ptrdiff_t find_first_of_neon(const char* p, std::size_t n,
                                  const char* set,
                                  std::size_t set_len) noexcept {
    uint8x16_t needles[8];
    for (std::size_t s = 0; s < set_len; ++s) {
        needles[s] = vdupq_n_u8(static_cast<uint8_t>(set[s]));
    }
    std::size_t off = 0;
    for (; off + 16 <= n; off += 16) {
        const uint8x16_t block =
            vld1q_u8(reinterpret_cast<const uint8_t*>(p) + off);
        uint8x16_t acc = vdupq_n_u8(0);
        for (std::size_t s = 0; s < set_len; ++s) {
            acc = vorrq_u8(acc, vceqq_u8(block, needles[s]));
        }
        const uint64x2_t h = vreinterpretq_u64_u8(acc);
        const uint64_t l = vgetq_lane_u64(h, 0);
        const uint64_t r = vgetq_lane_u64(h, 1);
        if (l != 0 || r != 0) {
            for (std::size_t k = 0; k < 16; ++k) {
                if (vgetq_lane_u8(acc, k) != 0) {
                    return static_cast<std::ptrdiff_t>(off + k);
                }
            }
        }
    }
    for (; off < n; ++off) {
        for (std::size_t s = 0; s < set_len; ++s) {
            if (p[off] == set[s]) {
                return static_cast<std::ptrdiff_t>(off);
            }
        }
    }
    return static_cast<std::ptrdiff_t>(n);
}

void dot_norms_neon(const float* a, const float* b, std::size_t n,
                    float& dot, float& na, float& nb) noexcept {
    float32x4_t d = vdupq_n_f32(0.0f);
    float32x4_t x = vdupq_n_f32(0.0f);
    float32x4_t y = vdupq_n_f32(0.0f);
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const float32x4_t av = vld1q_f32(a + i);
        const float32x4_t bv = vld1q_f32(b + i);
        d = vfmaq_f32(d, av, bv);
        x = vfmaq_f32(x, av, av);
        y = vfmaq_f32(y, bv, bv);
    }
    float sd = vaddvq_f32(d);
    float sx = vaddvq_f32(x);
    float sy = vaddvq_f32(y);
    for (; i < n; ++i) {
        sd += a[i] * b[i];
        sx += a[i] * a[i];
        sy += b[i] * b[i];
    }
    dot = sd;
    na = sx;
    nb = sy;
}

float sum_sq_diff_neon(const float* a, const float* b,
                       std::size_t n) noexcept {
    float32x4_t acc = vdupq_n_f32(0.0f);
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const float32x4_t d =
            vsubq_f32(vld1q_f32(a + i), vld1q_f32(b + i));
        acc = vfmaq_f32(acc, d, d);
    }
    float s = vaddvq_f32(acc);
    for (; i < n; ++i) {
        const float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}

#endif

void dot_norms_scalar(const float* a, const float* b, std::size_t n,
                      float& dot, float& na, float& nb) noexcept {
    float d = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        d += a[i] * b[i];
        x += a[i] * a[i];
        y += b[i] * b[i];
    }
    dot = d;
    na = x;
    nb = y;
}

float sum_sq_diff_scalar(const float* a, const float* b,
                         std::size_t n) noexcept {
    float s = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        const float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}

inline float cosine_from_norms(float dot, float na, float nb) noexcept {
    const float denom = std::sqrt(na * nb);
    if (denom < 1e-12f) {
        return 0.0f;
    }
    return dot / denom;
}

}

SimdLevel simd_level() noexcept {
    static const SimdLevel level = []() noexcept {
#if defined(DDCI_HAS_X86_TARGET)
        if (__builtin_cpu_supports("avx512f") &&
            __builtin_cpu_supports("avx512bw")) {
            return SimdLevel::Avx512;
        }
        if (__builtin_cpu_supports("avx2")) {
            return SimdLevel::Avx2;
        }
#endif
#if defined(DDCI_HAS_NEON)
        return SimdLevel::Neon;
#endif
        return SimdLevel::Scalar;
    }();
    return level;
}

const char* simd_level_name() noexcept {
    switch (simd_level()) {
    case SimdLevel::Avx512:
        return "avx512";
    case SimdLevel::Avx2:
        return "avx2";
    case SimdLevel::Neon:
        return "neon";
    case SimdLevel::Scalar:
    default:
        return "scalar";
    }
}

std::ptrdiff_t simd_find_byte(const char* data, std::size_t n,
                              char c) noexcept {
    if (n == 0) {
        return 0;
    }
    switch (simd_level()) {
#if defined(DDCI_HAS_X86_TARGET)
    case SimdLevel::Avx512:
        return find_byte_avx512(data, n, c);
    case SimdLevel::Avx2:
        return find_byte_avx2(data, n, c);
#endif
#if defined(DDCI_HAS_NEON)
    case SimdLevel::Neon:
        return find_byte_neon(data, n, c);
#endif
    case SimdLevel::Scalar:
    default:
        return find_byte_scalar(data, n, c);
    }
}

std::size_t simd_count_byte(const char* data, std::size_t n, char c) noexcept {
    if (n == 0) {
        return 0;
    }
    switch (simd_level()) {
#if defined(DDCI_HAS_X86_TARGET)
    case SimdLevel::Avx512:
        return count_byte_avx512(data, n, c);
    case SimdLevel::Avx2:
        return count_byte_avx2(data, n, c);
#endif
#if defined(DDCI_HAS_NEON)
    case SimdLevel::Neon:
        return count_byte_neon(data, n, c);
#endif
    case SimdLevel::Scalar:
    default:
        return count_byte_scalar(data, n, c);
    }
}

std::ptrdiff_t simd_find_first_of(const char* data, std::size_t n,
                                  const char* set,
                                  std::size_t set_len) noexcept {
    if (n == 0 || set_len == 0) {
        return static_cast<std::ptrdiff_t>(n);
    }
    if (set_len > 8) {
        return find_first_of_scalar(data, n, set, set_len);
    }
    switch (simd_level()) {
#if defined(DDCI_HAS_X86_TARGET)
    case SimdLevel::Avx512:
        return find_first_of_avx512(data, n, set, set_len);
    case SimdLevel::Avx2:
        return find_first_of_avx2(data, n, set, set_len);
#endif
#if defined(DDCI_HAS_NEON)
    case SimdLevel::Neon:
        return find_first_of_neon(data, n, set, set_len);
#endif
    case SimdLevel::Scalar:
    default:
        return find_first_of_scalar(data, n, set, set_len);
    }
}

std::ptrdiff_t simd_find_ws(const char* data, std::size_t n) noexcept {
    return simd_find_first_of(data, n, kWsSet, kWsSetLen);
}

std::string_view simd_next_token(const char* data, std::size_t n,
                                 std::size_t& pos) noexcept {
    if (pos >= n) {
        return {};
    }
    std::size_t p = pos;
    while (p < n && is_ws(data[p])) {
        ++p;
    }
    if (p >= n) {
        pos = n;
        return {};
    }
    const std::ptrdiff_t rel = simd_find_ws(data + p, n - p);
    const std::size_t end =
        (rel < 0) ? n : p + static_cast<std::size_t>(rel);
    pos = end;
    return std::string_view(data + p, end - p);
}

float simd_cosine_similarity(const float* a, const float* b,
                             std::size_t n) noexcept {
    if (n == 0) {
        return 0.0f;
    }
    float dot = 0.0f;
    float na = 1.0f;
    float nb = 1.0f;
    switch (simd_level()) {
#if defined(DDCI_HAS_X86_TARGET)
    case SimdLevel::Avx512:
        dot_norms_avx512(a, b, n, dot, na, nb);
        break;
    case SimdLevel::Avx2:
        dot_norms_avx2(a, b, n, dot, na, nb);
        break;
#endif
#if defined(DDCI_HAS_NEON)
    case SimdLevel::Neon:
        dot_norms_neon(a, b, n, dot, na, nb);
        break;
#endif
    case SimdLevel::Scalar:
    default:
        dot_norms_scalar(a, b, n, dot, na, nb);
        break;
    }
    return cosine_from_norms(dot, na, nb);
}

float simd_l2_distance(const float* a, const float* b,
                       std::size_t n) noexcept {
    if (n == 0) {
        return 0.0f;
    }
    float s = 0.0f;
    switch (simd_level()) {
#if defined(DDCI_HAS_X86_TARGET)
    case SimdLevel::Avx512:
        s = sum_sq_diff_avx512(a, b, n);
        break;
    case SimdLevel::Avx2:
        s = sum_sq_diff_avx2(a, b, n);
        break;
#endif
#if defined(DDCI_HAS_NEON)
    case SimdLevel::Neon:
        s = sum_sq_diff_neon(a, b, n);
        break;
#endif
    case SimdLevel::Scalar:
    default:
        s = sum_sq_diff_scalar(a, b, n);
        break;
    }
    return std::sqrt(s);
}

bool simd_verify_similar(const float* a, const float* b, std::size_t n,
                         float threshold) noexcept {
    return simd_cosine_similarity(a, b, n) >= threshold;
}

bool simd_selftest() noexcept {
    const char text[] =
        "alpha beta gamma\n\t delta epsilon  omega\r\n";
    const std::size_t tn = sizeof(text) - 1;
    const std::string_view tv(text, tn);

    const std::size_t expect_first_n = tv.find('n');
    const std::size_t expect_count_n = 1;
    const std::size_t expect_first_ws = tv.find_first_of(" \t\n\r\v\f");

    bool ok = true;

    if (simd_find_byte(text, tn, 'n') !=
        static_cast<std::ptrdiff_t>(expect_first_n)) {
        ok = false;
    }
    if (simd_count_byte(text, tn, 'n') != expect_count_n) {
        ok = false;
    }
    if (simd_find_first_of(text, tn, " \t", 2) !=
        static_cast<std::ptrdiff_t>(tv.find_first_of(" \t"))) {
        ok = false;
    }
    if (simd_find_ws(text, tn) !=
        static_cast<std::ptrdiff_t>(expect_first_ws)) {
        ok = false;
    }
    if (simd_find_byte(text, tn, 'Z') != static_cast<std::ptrdiff_t>(tn)) {
        ok = false;
    }
    if (simd_count_byte(text, tn, 'Z') != 0) {
        ok = false;
    }

    {
        std::size_t pos = 0;
        std::size_t k = 0;
        std::string_view tok;
        while (!tok.empty() || k == 0) {
            tok = simd_next_token(text, tn, pos);
            if (tok.empty()) {
                break;
            }
            const std::size_t sp = tv.find_first_not_of(" \t\n\r\v\f", k);
            const std::size_t ep = tv.find_first_of(" \t\n\r\v\f", sp);
            const std::size_t elen = (ep == std::string_view::npos)
                                         ? tn - sp
                                         : ep - sp;
            if (tok != tv.substr(sp, elen)) {
                ok = false;
                break;
            }
            k = (ep == std::string_view::npos) ? tn : ep;
        }
    }

    alignas(64) float a[256];
    alignas(64) float b[256];
    for (std::size_t i = 0; i < 256; ++i) {
        a[i] = static_cast<float>(0.5 + 0.01 * static_cast<double>(i % 61));
        b[i] = static_cast<float>(-0.3 + 0.02 * static_cast<double>((i * 7) % 89));
    }
    double dd = 0.0;
    double da = 0.0;
    double db = 0.0;
    for (std::size_t i = 0; i < 256; ++i) {
        dd += static_cast<double>(a[i]) * b[i];
        da += static_cast<double>(a[i]) * a[i];
        db += static_cast<double>(b[i]) * b[i];
    }
    const float ref_cos =
        static_cast<float>(dd / std::sqrt(da * db));
    const float got_cos = simd_cosine_similarity(a, b, 256);
    if (std::fabs(got_cos - ref_cos) > 5e-4f) {
        ok = false;
    }
    double dl2 = 0.0;
    for (std::size_t i = 0; i < 256; ++i) {
        const double d = static_cast<double>(a[i]) - b[i];
        dl2 += d * d;
    }
    const float ref_l2 = static_cast<float>(std::sqrt(dl2));
    if (std::fabs(simd_l2_distance(a, b, 256) - ref_l2) > 5e-3f) {
        ok = false;
    }
    if (!simd_verify_similar(a, a, 256, 0.999f)) {
        ok = false;
    }
    if (simd_verify_similar(a, b, 256, 2.0f)) {
        ok = false;
    }

    char probe[40];
    for (std::size_t i = 0; i < sizeof(probe); ++i) {
        probe[i] = static_cast<char>('a' + (i % 26));
    }
    probe[3] = 'n';
    probe[17] = 'n';
    probe[38] = 'n';
    const std::size_t pn = sizeof(probe);

#if defined(DDCI_HAS_X86_TARGET)
    if (__builtin_cpu_supports("avx512f") &&
        __builtin_cpu_supports("avx512bw")) {
        if (find_byte_avx512(probe, pn, 'n') !=
            find_byte_scalar(probe, pn, 'n')) {
            ok = false;
        }
        if (count_byte_avx512(probe, pn, 'n') !=
            count_byte_scalar(probe, pn, 'n')) {
            ok = false;
        }
        if (find_first_of_avx512(probe, pn, "aeiou", 5) !=
            find_first_of_scalar(probe, pn, "aeiou", 5)) {
            ok = false;
        }
        float dd0, dda, ddb;
        float dd1, dda1, ddb1;
        dot_norms_avx512(a, b, 256, dd0, dda, ddb);
        dot_norms_scalar(a, b, 256, dd1, dda1, ddb1);
        if (std::fabs(dd0 - dd1) > 5e-3f ||
            std::fabs(dda - dda1) > 5e-2f ||
            std::fabs(ddb - ddb1) > 5e-2f) {
            ok = false;
        }
        if (std::fabs(sum_sq_diff_avx512(a, b, 256) -
                      sum_sq_diff_scalar(a, b, 256)) > 5e-2f) {
            ok = false;
        }
    }
    if (__builtin_cpu_supports("avx2")) {
        if (find_byte_avx2(probe, pn, 'n') !=
            find_byte_scalar(probe, pn, 'n')) {
            ok = false;
        }
        if (count_byte_avx2(probe, pn, 'n') !=
            count_byte_scalar(probe, pn, 'n')) {
            ok = false;
        }
        if (find_first_of_avx2(probe, pn, "aeiou", 5) !=
            find_first_of_scalar(probe, pn, "aeiou", 5)) {
            ok = false;
        }
        float dd0, dda, ddb;
        float dd1, dda1, ddb1;
        dot_norms_avx2(a, b, 256, dd0, dda, ddb);
        dot_norms_scalar(a, b, 256, dd1, dda1, ddb1);
        if (std::fabs(dd0 - dd1) > 5e-3f ||
            std::fabs(dda - dda1) > 5e-2f ||
            std::fabs(ddb - ddb1) > 5e-2f) {
            ok = false;
        }
        if (std::fabs(sum_sq_diff_avx2(a, b, 256) -
                      sum_sq_diff_scalar(a, b, 256)) > 5e-2f) {
            ok = false;
        }
    }
#endif
#if defined(DDCI_HAS_NEON)
    {
        if (find_byte_neon(probe, pn, 'n') !=
            find_byte_scalar(probe, pn, 'n')) {
            ok = false;
        }
        if (count_byte_neon(probe, pn, 'n') !=
            count_byte_scalar(probe, pn, 'n')) {
            ok = false;
        }
        if (find_first_of_neon(probe, pn, "aeiou", 5) !=
            find_first_of_scalar(probe, pn, "aeiou", 5)) {
            ok = false;
        }
        float dd0, dda, ddb;
        float dd1, dda1, ddb1;
        dot_norms_neon(a, b, 256, dd0, dda, ddb);
        dot_norms_scalar(a, b, 256, dd1, dda1, ddb1);
        if (std::fabs(dd0 - dd1) > 5e-3f ||
            std::fabs(dda - dda1) > 5e-2f ||
            std::fabs(ddb - ddb1) > 5e-2f) {
            ok = false;
        }
        if (std::fabs(sum_sq_diff_neon(a, b, 256) -
                      sum_sq_diff_scalar(a, b, 256)) > 5e-2f) {
            ok = false;
        }
    }
#endif

    return ok;
}

}