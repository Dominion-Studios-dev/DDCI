#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ddci::memory {

inline constexpr std::size_t kEmbedDim = 512;
inline constexpr std::size_t kDefaultCacheCapacity = 128;
inline constexpr float kSemanticThreshold = 0.95f;

inline constexpr std::size_t kMaxKeyChars = 2048;
inline constexpr std::size_t kMaxResponseChars = 4096;

[[nodiscard]] float avx2_cosine_similarity(const float* a, const float* b,
                                           std::size_t dim) noexcept;

class SemanticCache {
public:
    struct Hit {
        float similarity;
        std::string response;
    };

    explicit SemanticCache(std::size_t capacity = kDefaultCacheCapacity);
    ~SemanticCache() = default;

    SemanticCache(const SemanticCache&) = delete;
    SemanticCache& operator=(const SemanticCache&) = delete;
    SemanticCache(SemanticCache&&) noexcept = default;
    SemanticCache& operator=(SemanticCache&&) noexcept = default;

    void embed(std::string_view text, float* out) const noexcept;

    [[nodiscard]] std::optional<Hit> lookup(const float* query);

    [[nodiscard]] std::optional<Hit> find(std::string_view prompt);

    std::size_t store(std::string_view prompt, std::string response);

    void reset() noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t hit_count() const noexcept { return hits_; }
    [[nodiscard]] std::size_t miss_count() const noexcept { return misses_; }
    [[nodiscard]] std::size_t dropped_count() const noexcept { return dropped_; }

private:
    struct alignas(64) Entry {
        alignas(64) float embedding[kEmbedDim];
        std::string prompt;
        std::string response;
    };

    void do_store(std::size_t slot, const float* embedding,
                  std::string_view prompt, std::string response);

    std::unique_ptr<Entry[]> pool_;
    std::size_t capacity_;
    std::size_t size_{0};

    std::size_t hits_{0};
    std::size_t misses_{0};
    std::size_t dropped_{0};
};

}
