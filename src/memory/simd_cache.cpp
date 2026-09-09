#include "memory/simd_cache.hpp"

#include <immintrin.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

namespace ddci::memory {

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

inline bool is_alnum_ascii(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

inline char lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::uint64_t fnv1a_bytes(const unsigned char* p,
                                 std::size_t n) noexcept {
    std::uint64_t h = kFnvOffsetBasis;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
    return h;
}

inline void add_feature(float* v, std::uint64_t h, float w) noexcept {
    const float s = (h & (1ULL << 63)) ? -w : w;
    v[h % kEmbedDim] += s;
}

inline constexpr float kStopwordWeight = 0.25f;

inline bool is_stopword_lower(const unsigned char* t,
                              std::size_t len) noexcept {
    static constexpr std::string_view kStop[] = {
        "what", "which", "why", "how", "who", "where", "when",
        "is", "are", "was", "were", "be", "been", "being",
        "the", "a", "an", "of", "to", "in", "on", "for",
        "and", "or", "but", "with", "about", "by", "at", "from",
        "it", "its", "this", "that", "these", "those",
        "do", "does", "did", "can", "could", "will", "would", "should",
        "have", "has", "had", "not", "you", "your", "me", "my", "i"};
    for (const auto w : kStop) {
        if (w.size() == len &&
            std::memcmp(w.data(), t, len) == 0) {
            return true;
        }
    }
    return false;
}

inline float reduce8(__m256 x) noexcept {
    __m128 lo = _mm256_castps256_ps128(x);
    __m128 hi = _mm256_extractf128_ps(x, 1);
    __m128 s = _mm_add_ps(lo, hi);
    __m128 h1 = _mm_hadd_ps(s, s);
    __m128 h2 = _mm_hadd_ps(h1, h1);
    return _mm_cvtss_f32(h2);
}

}

float avx2_cosine_similarity(const float* a, const float* b,
                             std::size_t dim) noexcept {
    if (dim == 0) {
        return 0.0f;
    }

    __m256 dot = _mm256_setzero_ps();
    __m256 na = _mm256_setzero_ps();
    __m256 nb = _mm256_setzero_ps();

    std::size_t i = 0;
    for (; i + 8 <= dim; i += 8) {
        const __m256 av = _mm256_load_ps(a + i);
        const __m256 bv = _mm256_load_ps(b + i);
        dot = _mm256_fmadd_ps(av, bv, dot);
        na = _mm256_fmadd_ps(av, av, na);
        nb = _mm256_fmadd_ps(bv, bv, nb);
    }

    float d = reduce8(dot);
    float da = reduce8(na);
    float db = reduce8(nb);
    for (; i < dim; ++i) {
        d += a[i] * b[i];
        da += a[i] * a[i];
        db += b[i] * b[i];
    }

    const float denom = std::sqrt(da * db);
    if (denom < 1e-12f) {
        return 0.0f;
    }
    return d / denom;
}

SemanticCache::SemanticCache(std::size_t capacity)
    : capacity_(capacity == 0 ? kDefaultCacheCapacity : capacity) {
    pool_ = std::make_unique<Entry[]>(capacity_);
}

void SemanticCache::embed(std::string_view text, float* out) const noexcept {
    std::memset(out, 0, kEmbedDim * sizeof(float));
    if (text.empty()) {
        return;
    }

    const char* p = text.data();
    const std::size_t n = text.size();

    std::uint64_t prev_word_hash = 0;
    bool have_prev = false;
    bool prev_content = false;

    std::size_t i = 0;
    while (i < n) {
        while (i < n && !is_alnum_ascii(p[i])) {
            ++i;
        }
        if (i >= n) {
            break;
        }
        const std::size_t start = i;
        while (i < n && is_alnum_ascii(p[i])) {
            ++i;
        }
        const std::size_t wlen = i - start;

        unsigned char tok[80];
        const std::size_t tlen =
            (wlen <= sizeof(tok)) ? wlen : sizeof(tok);
        for (std::size_t k = 0; k < tlen; ++k) {
            tok[k] = static_cast<unsigned char>(lower_ascii(p[start + k]));
        }

        const bool content = !is_stopword_lower(tok, tlen);
        const std::uint64_t wh = fnv1a_bytes(tok, tlen);
        if (content) {
            add_feature(out, wh, 2.0f);
            for (std::size_t k = 0; k + 1 < tlen; ++k) {
                add_feature(out, fnv1a_bytes(tok + k, 2), 1.0f);
            }
            for (std::size_t k = 0; k + 2 < tlen; ++k) {
                add_feature(out, fnv1a_bytes(tok + k, 3), 0.5f);
            }
        } else {
            add_feature(out, wh, kStopwordWeight);
        }
        if (have_prev && (prev_content || content)) {
            add_feature(out, (prev_word_hash ^ wh) * 0x9E3779B97F4A7C15ULL,
                        1.0f);
        }
        prev_word_hash = wh;
        prev_content = content;
        have_prev = true;
    }

    float ss = 0.0f;
    for (std::size_t d = 0; d < kEmbedDim; ++d) {
        ss += out[d] * out[d];
    }
    if (ss <= 1e-12f) {
        return;
    }
    const float inv = 1.0f / std::sqrt(ss);
    for (std::size_t d = 0; d < kEmbedDim; ++d) {
        out[d] *= inv;
    }
}

std::optional<SemanticCache::Hit> SemanticCache::lookup(
    const float* query) {
    const std::size_t n = size_;

    float best = -1.0f;
    std::size_t best_idx = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const float s =
            avx2_cosine_similarity(query, pool_[i].embedding, kEmbedDim);
        if (s > best) {
            best = s;
            best_idx = i;
        }
    }

    if (best < kSemanticThreshold) {
        ++misses_;
        return std::nullopt;
    }

    ++hits_;
    Hit h;
    h.similarity = best;
    h.response = pool_[best_idx].response;
    return h;
}

std::optional<SemanticCache::Hit> SemanticCache::find(
    std::string_view prompt) {
    alignas(64) float qv[kEmbedDim];
    embed(prompt, qv);
    return lookup(qv);
}

std::size_t SemanticCache::store(std::string_view prompt,
                                 std::string response) {
    if (size_ >= capacity_) {
        ++dropped_;
        return capacity_;
    }
    alignas(64) float qv[kEmbedDim];
    embed(prompt, qv);
    do_store(size_, qv, prompt, std::move(response));
    return size_ - 1;
}

void SemanticCache::do_store(std::size_t slot, const float* embedding,
                             std::string_view prompt,
                             std::string response) {
    Entry& e = pool_[slot];
    std::memcpy(e.embedding, embedding, kEmbedDim * sizeof(float));
    e.prompt.assign(prompt);
    e.response = std::move(response);
    size_ = slot + 1;
}

void SemanticCache::reset() noexcept {
    size_ = 0;
    dropped_ = 0;
}

}
