#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace ddci::core {

enum class ErrorCode : uint8_t {
    Success = 0,
    DatabaseError,
    NetworkError,
    ConfigError,
    MemoryError
};

template <typename T>
class Result {
public:
    Result(const Result&) = default;
    Result& operator=(const Result&) = default;

    Result(Result&&) noexcept = default;
    Result& operator=(Result&&) noexcept = default;

    Result(T val) : data_(std::move(val)) {}

    static Result failure(ErrorCode code) {
        return Result(code);
    }

    static Result success(T val) {
        return Result(std::move(val));
    }

    [[nodiscard]] bool ok() const {
        return std::holds_alternative<T>(data_);
    }

    [[nodiscard]] const T& value() const& {
        return std::get<T>(data_);
    }

    [[nodiscard]] T&& value() && {
        return std::get<T>(std::move(data_));
    }

    [[nodiscard]] ErrorCode error() const {
        return std::get<ErrorCode>(data_);
    }

private:
    explicit Result(ErrorCode code) : data_(code) {}
    Result() = default;
    std::variant<T, ErrorCode> data_{ErrorCode::Success};
};

template <>
class Result<void> {
public:
    Result() : error_(ErrorCode::Success) {}

    Result(const Result&) = default;
    Result& operator=(const Result&) = default;
    Result(Result&&) noexcept = default;
    Result& operator=(Result&&) noexcept = default;

    static Result failure(ErrorCode code) {
        Result r;
        r.error_ = code;
        return r;
    }

    static Result success() {
        return Result();
    }

    [[nodiscard]] bool ok() const {
        return error_ == ErrorCode::Success;
    }

    [[nodiscard]] ErrorCode error() const {
        return error_;
    }

private:
    ErrorCode error_;
};

}