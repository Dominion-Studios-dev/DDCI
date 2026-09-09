#include "ddci/net/timing_log.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <utility>

namespace ddci::net {

namespace {

inline void write_timestamp(char* buf, std::size_t cap) noexcept {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    const std::size_t n = std::strftime(buf, cap, "%Y-%m-%dT%H:%M:%S", &tm);
    if (n == 0) {
        buf[0] = '\0';
    }
}

inline constexpr std::string_view kHeader =
    "timestamp,model,stream,cache_hit,connection_reused,prefix_digest,"
    "prefix_bytes,queue_us,dns_us,connect_us,tls_us,setup_us,ttfb_us,"
    "ttft_us,generation_us,total_us,prompt_tokens,completion_tokens,"
    "total_tokens,response_bytes,success\n";

}

TimingLog::TimingLog(std::string path)
    : path_(std::move(path)) {
    constexpr mode_t kLedgerMode = S_IRUSR | S_IWUSR;
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                 kLedgerMode);
    if (fd_ < 0) {
        return;
    }
    (void)::fchmod(fd_, kLedgerMode);
    struct stat st {};
    if (::fstat(fd_, &st) == 0 && st.st_size == 0) {
        append(kHeader.data(), kHeader.size());
    }
}

void TimingLog::append(const char* data, std::size_t n) noexcept {
    std::size_t off = 0;
    while (off < n) {
        const ssize_t w = ::write(fd_, data + off, n - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        off += static_cast<std::size_t>(w);
    }
}

void TimingLog::record(const RequestTiming& t) noexcept {
    if (fd_ < 0) {
        return;
    }

    char row[1024];
    char ts[40];
    write_timestamp(ts, sizeof(ts));

    const std::uint64_t total_tokens = t.prompt_tokens + t.completion_tokens;
    const int n = std::snprintf(
        row, sizeof(row),
        "%s,%.*s,%d,%d,%d,%llu,%llu,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
        "%lld,%lld,%llu,%llu,%llu,%llu,%d\n",
        ts,
        static_cast<int>(t.model.size()), t.model.data(),
        static_cast<int>(t.stream),
        static_cast<int>(t.cache_hit),
        static_cast<int>(t.connection_reused),
        static_cast<unsigned long long>(t.prefix_digest),
        static_cast<unsigned long long>(t.prefix_bytes),
        static_cast<long long>(t.queue_us),
        static_cast<long long>(t.dns_us),
        static_cast<long long>(t.connect_us),
        static_cast<long long>(t.tls_us),
        static_cast<long long>(t.setup_us),
        static_cast<long long>(t.ttfb_us),
        static_cast<long long>(t.ttft_us),
        static_cast<long long>(t.generation_us),
        static_cast<long long>(t.total_us),
        static_cast<unsigned long long>(t.prompt_tokens),
        static_cast<unsigned long long>(t.completion_tokens),
        static_cast<unsigned long long>(total_tokens),
        static_cast<unsigned long long>(t.response_bytes),
        static_cast<int>(t.success));

    if (n <= 0) {
        return;
    }
    const std::size_t len = static_cast<std::size_t>(n);
    if (len >= sizeof(row)) {
        return;
    }
    append(row, len);
    ++rows_;
}

void TimingLog::flush() noexcept {
    if (fd_ >= 0) {
        ::fsync(fd_);
    }
}

}
