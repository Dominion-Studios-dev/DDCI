#include "telemetry/mmap_logger.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace ddci::telemetry {

MmapLogger::MmapLogger(std::string file_path)
    : file_path_(std::move(file_path)) {
    constexpr mode_t kLedgerMode = S_IRUSR | S_IWUSR;
    fd_ = ::open(file_path_.c_str(),
                 O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                 kLedgerMode);
    if (fd_ < 0) {
        return;
    }
    (void)::fchmod(fd_, kLedgerMode);

    struct stat st{};
    if (::fstat(fd_, &st) == 0) {
        write_offset_ = static_cast<std::size_t>(st.st_size);
    }
}

MmapLogger::~MmapLogger() {
    if (fd_ >= 0) {
        (void)::fsync(fd_);
        (void)::close(fd_);
        fd_ = -1;
    }
}

MmapLogger::MmapLogger(MmapLogger&& other) noexcept
    : file_path_(std::move(other.file_path_))
    , fd_(other.fd_)
    , write_offset_(other.write_offset_) {
    other.fd_ = -1;
    other.write_offset_ = 0;
}

MmapLogger& MmapLogger::operator=(MmapLogger&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
        file_path_ = std::move(other.file_path_);
        fd_ = other.fd_;
        write_offset_ = other.write_offset_;
        other.fd_ = -1;
        other.write_offset_ = 0;
    }
    return *this;
}

core::Result<void> MmapLogger::append(std::string_view record) noexcept {
    if (record.empty()) {
        return core::Result<void>::success();
    }
    if (fd_ < 0) {
        return core::Result<void>::failure(core::ErrorCode::MemoryError);
    }

    const char* p = record.data();
    std::size_t remaining = record.size();
    while (remaining > 0) {
        const ssize_t n = ::write(fd_, p, remaining);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return core::Result<void>::failure(core::ErrorCode::MemoryError);
        }
        if (n == 0) {
            return core::Result<void>::failure(core::ErrorCode::MemoryError);
        }
        p += static_cast<std::size_t>(n);
        remaining -= static_cast<std::size_t>(n);
    }

    write_offset_ += record.size();
    return core::Result<void>::success();
}

core::Result<void> MmapLogger::flush() noexcept {
    if (fd_ < 0) {
        return core::Result<void>::failure(core::ErrorCode::MemoryError);
    }
    if (::fsync(fd_) != 0) {
        return core::Result<void>::failure(core::ErrorCode::MemoryError);
    }
    return core::Result<void>::success();
}

}
