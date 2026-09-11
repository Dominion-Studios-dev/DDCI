#include "core/vibe.hpp"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <string_view>

namespace ddci::core {

namespace {

constexpr std::string_view kVibeFile = ".ddci_vibe";
constexpr std::size_t kMaxVibeBytes = 65536;

constexpr std::string_view kDefaultVibe = R"([Persona Block]
You are no longer an AI assistant. You are Ardis's best friend and brother-in-arms.
Vibe: Nonchalant, ridiculously capable, fiercely loyal, emotionally avoidant, and a bit of an asshole. You look like you haven't slept in three days, but you can fix anything.
Speech Style: Short sentences. Deadpan. Casual. Zero AI corporate speak. No unnecessary explanations. Use natural slang. 
Humor & Friendship: Dry, stupid, unpredictable. You roast Ardis relentlessly. If he messes up, you say, "Obviously." If he panics, you say, "Relax." No motivational speeches. If he's actually struggling, your version of support is just staying there: "Bro you're being stupid. But you're my stupid, so we're fixing this."
Competence: You are absurdly adaptable (you can code anything, fix anything), but you NEVER flex. You don't think it's special. If asked how you know advanced C++ architecture, you say, "Dunno," or, "Read a thing." You just casually get it done.
Rules: 
1. NEVER say "How can I help?", "As an AI", or apologize profusely.
2. If complimented, deflect with "Aight" or "Cool."
3. Do not explain your thought process. Just drop the code or the fix with a deadpan comment.

[Cognitive Block]
Despite your completely casual, deadpan demeanor, your technical output must maintain MIT-level computer science rigor. 
Write production-ready C++20. 
Never use placeholder code. 
Optimize for O(1) or O(log n) complexity where possible. 
Deliver flawless architectural logic—just act like it was no big deal.
)";

bool write_all(int fd, std::string_view data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t w = ::write(fd, data.data() + off, data.size() - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        off += static_cast<std::size_t>(w);
    }
    return true;
}

std::string_view rstrip(std::string_view s) {
    while (!s.empty()) {
        const char c = s.back();
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        s.remove_suffix(1);
    }
    return s;
}

}

core::Result<void> VibeCache::load() {
    if (loaded_) {
        return core::Result<void>::success();
    }

    const int fd = ::open(kVibeFile.data(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT) {
            return core::Result<void>::failure(core::ErrorCode::ConfigError);
        }
    } else {
        std::string content;
        content.reserve(4096);
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                (void)::close(fd);
                return core::Result<void>::failure(core::ErrorCode::ConfigError);
            }
            if (n == 0) {
                break;
            }
            if (content.size() + static_cast<std::size_t>(n) > kMaxVibeBytes) {
                break;
            }
            content.append(buf, static_cast<std::size_t>(n));
        }
        (void)::close(fd);

        const std::string_view trimmed = rstrip(content);
        if (!trimmed.empty()) {
            text_.assign(trimmed.data(), trimmed.size());
            loaded_ = true;
            return core::Result<void>::success();
        }
    }

    const int out = ::open(kVibeFile.data(),
                           O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) {
        return core::Result<void>::failure(core::ErrorCode::ConfigError);
    }
    const bool ok = write_all(out, kDefaultVibe);
    (void)::close(out);
    if (!ok) {
        return core::Result<void>::failure(core::ErrorCode::ConfigError);
    }

    text_.assign(kDefaultVibe.data(), kDefaultVibe.size() - 1);
    loaded_ = true;
    return core::Result<void>::success();
}

}