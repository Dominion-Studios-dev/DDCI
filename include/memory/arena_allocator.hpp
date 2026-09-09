#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string_view>
#include <utility>

namespace ddci::memory {

class alignas(64) ArenaAllocator {
public:
    explicit ArenaAllocator(std::size_t capacity = 1u << 20) noexcept {
        capacity_ = capacity != 0 ? capacity : 1;
        base_ = static_cast<char*>(
            ::operator new(capacity_, std::align_val_t{kAlignment},
                           std::nothrow));
        cur_ = base_;
        end_ = base_ != nullptr ? base_ + capacity_ : nullptr;
    }

    ~ArenaAllocator() {
        release();
    }

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    ArenaAllocator(ArenaAllocator&& other) noexcept
        : base_(other.base_), cur_(other.cur_), end_(other.end_),
          capacity_(other.capacity_) {
        other.base_  = nullptr;
        other.cur_   = nullptr;
        other.end_   = nullptr;
        other.capacity_ = 0;
    }

    ArenaAllocator& operator=(ArenaAllocator&& other) noexcept {
        if (this != &other) {
            release();
            base_     = other.base_;
            cur_      = other.cur_;
            end_      = other.end_;
            capacity_ = other.capacity_;
            other.base_  = nullptr;
            other.cur_   = nullptr;
            other.end_   = nullptr;
            other.capacity_ = 0;
        }
        return *this;
    }

    void reset() noexcept {
        cur_ = base_;
    }

    [[nodiscard]] void* alloc(std::size_t bytes,
                              std::size_t align = kAlignment) noexcept {
        if (base_ == nullptr) {
            return nullptr;
        }
        const std::uintptr_t cur_addr =
            reinterpret_cast<std::uintptr_t>(cur_);
        const std::uintptr_t a = align != 0 ? align : kAlignment;
        const std::uintptr_t aligned_addr =
            (cur_addr + a - 1) & ~(a - 1);
        const std::size_t shift =
            static_cast<std::size_t>(aligned_addr - cur_addr);
        if (shift + bytes > (size_remaining())) {
            return nullptr;
        }
        char* result = reinterpret_cast<char*>(aligned_addr);
        cur_ = result + bytes;
        return result;
    }

    [[nodiscard]] std::string_view alloc_string(
        std::string_view source) noexcept {
        if (source.empty() || base_ == nullptr) {
            return std::string_view{};
        }
        void* p = alloc(source.size(), 1);
        if (p == nullptr) {
            return std::string_view{};
        }
        std::memcpy(p, source.data(), source.size());
        return std::string_view(static_cast<const char*>(p), source.size());
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return base_ != nullptr ? size_remaining() : 0;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    [[nodiscard]] std::size_t size_remaining() const noexcept {
        return static_cast<std::size_t>(end_ - cur_);
    }

    void release() noexcept {
        if (base_ != nullptr) {
            ::operator delete(base_, capacity_, std::align_val_t{kAlignment});
            base_ = nullptr;
            cur_  = nullptr;
            end_  = nullptr;
        }
    }

    static constexpr std::size_t kAlignment = 64;

    char* base_{nullptr};
    char* cur_{nullptr};
    char* end_{nullptr};
    std::size_t capacity_{0};
};

}
