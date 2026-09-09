#pragma once

#include "core/error.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace ddci::telemetry {

class MmapLogger {
public:
    explicit MmapLogger(std::string file_path);

    ~MmapLogger();

    MmapLogger(const MmapLogger&) = delete;
    MmapLogger& operator=(const MmapLogger&) = delete;

    MmapLogger(MmapLogger&&) noexcept;
    MmapLogger& operator=(MmapLogger&&) noexcept;

    [[nodiscard]] core::Result<void> append(std::string_view record) noexcept;

    [[nodiscard]] core::Result<void> flush() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

    [[nodiscard]] const std::string& path() const noexcept { return file_path_; }

    [[nodiscard]] std::size_t bytes_written() const noexcept {
        return write_offset_;
    }

private:
    std::string file_path_;
    int fd_{-1};
    std::size_t write_offset_{0};
};

}
