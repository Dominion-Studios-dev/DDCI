#include "workspace/workspace_context.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string_view>
#include <utility>

namespace ddci::workspace {

namespace {

bool run_capture(char* const argv[], char* buf, std::size_t cap,
                 std::size_t& out_len) {
    int fd[2] = { -1, -1 };
    if (::pipe(fd) != 0) {
        return false;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        (void)::close(fd[0]);
        (void)::close(fd[1]);
        return false;
    }
    if (pid == 0) {
        (void)::close(fd[0]);
        (void)::dup2(fd[1], STDOUT_FILENO);
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            (void)::dup2(devnull, STDERR_FILENO);
            (void)::close(devnull);
        }
        (void)::close(fd[1]);
        ::execvp("git", argv);
        ::_exit(127);
    }
    (void)::close(fd[1]);
    std::size_t total = 0;
    while (total < cap) {
        const ssize_t n = ::read(fd[0], buf + total, cap - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        total += static_cast<std::size_t>(n);
    }
    (void)::close(fd[0]);
    int status = 0;
    (void)::waitpid(pid, &status, 0);
    if (total < cap) {
        buf[total] = '\0';
    }
    out_len = total;
    return WIFEXITED(status) && (WEXITSTATUS(status) == 0);
}

std::string_view read_line(std::string_view out, std::size_t& pos) {
    if (pos >= out.size()) {
        return {};
    }
    const std::size_t eol = out.find('\n', pos);
    std::string_view line =
        out.substr(pos, (eol == std::string_view::npos) ? out.size() - pos
                                                        : eol - pos);
    pos = (eol == std::string_view::npos) ? out.size() : eol + 1;
    while (!line.empty() &&
           (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    return line;
}

}

GitState detect_git_state() {
    GitState state;
    char buf[16384];
    std::size_t len = 0;

    char* rev[] = {
        const_cast<char*>("git"),
        const_cast<char*>("rev-parse"),
        const_cast<char*>("--git-dir"),
        nullptr,
    };
    if (!run_capture(rev, buf, sizeof(buf), len)) {
        return state;
    }
    state.present = true;

    char branch[4096];
    std::size_t blen = 0;
    char* branch_args[] = {
        const_cast<char*>("git"),
        const_cast<char*>("branch"),
        const_cast<char*>("--show-current"),
        nullptr,
    };
    if (run_capture(branch_args, branch, sizeof(branch), blen)) {
        std::size_t pos = 0;
        const std::string_view b = read_line(branch, pos);
        if (!b.empty() && b.size() < sizeof(state.branch)) {
            std::memcpy(state.branch, b.data(), b.size());
            state.branch[b.size()] = '\0';
        }
    }

    char* status_args[] = {
        const_cast<char*>("git"),
        const_cast<char*>("status"),
        const_cast<char*>("--porcelain"),
        nullptr,
    };
    if (run_capture(status_args, buf, sizeof(buf), len)) {
        std::string_view out(buf, len);
        std::size_t pos = 0;
        std::string detail;
        unsigned shown = 0;
        for (;;) {
            const std::string_view line = read_line(out, pos);
            if (line.empty()) {
                if (pos >= out.size()) {
                    break;
                }
                continue;
            }
            if (line.size() < 4) {
                continue;
            }
            const char x = line[0];
            const char y = line[1];
            const std::string_view path = line.substr(3);
            if (x == '?' && y == '?') {
                ++state.untracked;
            } else {
                if (x != ' ' && x != '?') {
                    ++state.staged;
                }
                if (y != ' ' && y != '?') {
                    ++state.modified;
                }
            }
            if (shown < 3) {
                if (!detail.empty()) {
                    detail += ", ";
                }
                detail.append(path.data(), path.size());
                ++shown;
            }
        }
        if (detail.size() > 128) {
            detail.resize(128);
            detail += "\xe2\x80\xa6";
        }
        state.detail = std::move(detail);
    }

    return state;
}

}