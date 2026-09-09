#include "ddci/runtime/dag_executor.hpp"

#include <algorithm>
#include <mutex>

namespace ddci::runtime {

namespace {

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

inline std::size_t ctz64(std::uint64_t m) noexcept {
    return m == 0 ? 64u
                  : static_cast<std::size_t>(__builtin_ctzll(m));
}

}

static_assert(SpeculativeDAG::kRingSlots == 128,
              "MPMC ring size is fixed at 128 for the ticket scheme");

SpeculativeDAG::SpeculativeDAG(ddci::mem::ArenaScratchpad& pad) noexcept
    : pad_(pad) {
    for (std::uint32_t i = 0; i < kRingSlots; ++i) {
        ring_token_[i].store(kEmptyToken, std::memory_order_relaxed);
        ring_seq_[i].store(i, std::memory_order_relaxed);
    }
}

std::size_t SpeculativeDAG::add_node(std::uint64_t deps, TaskFn fn,
                                     void* user) noexcept {
    if (count_ >= kMaxNodes || fn == nullptr) {
        return kMaxNodes;
    }
    const std::size_t idx = count_;
    Node& n = nodes_[idx];
    n.deps = deps & ((std::uint64_t{1} << count_) - 1);
    n.fn = fn;
    n.user = user;
    n.state.store(static_cast<std::uint8_t>(NodeState::Pending),
                  std::memory_order_relaxed);

    std::uint64_t bits = n.deps;
    while (bits != 0) {
        const std::size_t d = ctz64(bits);
        bits &= bits - 1;
        consumers_[d] |= (std::uint64_t{1} << idx);
    }
    ++count_;
    return idx;
}

void SpeculativeDAG::set_output(std::size_t idx, const void* p,
                                std::size_t bytes) noexcept {
    if (idx < count_) {
        nodes_[idx].output = const_cast<void*>(p);
        nodes_[idx].output_bytes = bytes;
    }
}

const void* SpeculativeDAG::output(std::size_t idx) const noexcept {
    return (idx < count_) ? nodes_[idx].output : nullptr;
}

std::size_t SpeculativeDAG::output_bytes(std::size_t idx) const noexcept {
    return (idx < count_) ? nodes_[idx].output_bytes : 0;
}

bool SpeculativeDAG::is_done(std::size_t idx) const noexcept {
    if (idx >= count_) {
        return false;
    }
    return nodes_[idx].state.load(std::memory_order_acquire) ==
           static_cast<std::uint8_t>(NodeState::Done);
}

void SpeculativeDAG::enqueue(std::uint32_t token) noexcept {
    const std::uint64_t pos =
        enqueue_pos_.fetch_add(1, std::memory_order_relaxed);
    const std::size_t slot = static_cast<std::size_t>(pos) & kRingMask;
    std::atomic<std::uint32_t>& seq = ring_seq_[slot];
    while (seq.load(std::memory_order_acquire) !=
           static_cast<std::uint32_t>(pos)) {
        cpu_relax();
    }
    ring_token_[slot].store(token, std::memory_order_seq_cst);
    seq.store(static_cast<std::uint32_t>(pos + 1), std::memory_order_seq_cst);
}

bool SpeculativeDAG::try_dequeue(std::uint32_t& token) noexcept {
    std::uint64_t pos = dequeue_pos_.load(std::memory_order_acquire);
    for (;;) {
        const std::size_t slot = static_cast<std::size_t>(pos) & kRingMask;
        const std::uint32_t seqv =
            ring_seq_[slot].load(std::memory_order_acquire);
        if (seqv != static_cast<std::uint32_t>(pos + 1)) {
            return false;
        }
        if (dequeue_pos_.compare_exchange_weak(
                pos, pos + 1, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            token = ring_token_[slot].load(std::memory_order_seq_cst);
            ring_seq_[slot].store(static_cast<std::uint32_t>(pos + kRingSlots),
                                  std::memory_order_release);
            return true;
        }
    }
}

void SpeculativeDAG::runnable_or_done(std::size_t idx) noexcept {
    Node& n = nodes_[idx];
    const std::uint64_t deps = n.deps;
    if (((n.deps_seen.load(std::memory_order_acquire) & deps) != deps) ||
        n.state.load(std::memory_order_acquire) !=
            static_cast<std::uint8_t>(NodeState::Pending)) {
        return;
    }
    std::uint8_t expected = static_cast<std::uint8_t>(NodeState::Pending);
    if (n.state.compare_exchange_strong(
            expected, static_cast<std::uint8_t>(NodeState::Runnable),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        enqueue(static_cast<std::uint32_t>(idx));
    }
}

void SpeculativeDAG::process_node(std::size_t idx) noexcept {
    Node& n = nodes_[idx];

    std::uint8_t try_state = static_cast<std::uint8_t>(NodeState::Runnable);
    bool claimed = n.state.compare_exchange_strong(
        try_state, static_cast<std::uint8_t>(NodeState::Running),
        std::memory_order_acq_rel, std::memory_order_acquire);
    if (!claimed) {
        try_state = static_cast<std::uint8_t>(NodeState::Pending);
        claimed = n.state.compare_exchange_strong(
            try_state, static_cast<std::uint8_t>(NodeState::Running),
            std::memory_order_acq_rel, std::memory_order_acquire);
        if (!claimed) {
            return;
        }
    }

    n.fn(idx, n.user, *this, pad_);
    n.state.store(static_cast<std::uint8_t>(NodeState::Done),
                  std::memory_order_release);
    notify_dependents(idx);
    completed_.fetch_add(1, std::memory_order_acq_rel);
}

void SpeculativeDAG::notify_dependents(std::size_t idx) noexcept {
    const std::uint64_t bit = std::uint64_t{1} << idx;
    std::uint64_t bits = consumers_[idx];
    while (bits != 0) {
        const std::size_t j = ctz64(bits);
        bits &= bits - 1;
        Node& nj = nodes_[j];
        const std::uint64_t seen =
            nj.deps_seen.fetch_or(bit, std::memory_order_acq_rel) | bit;
        if ((seen & nj.deps) == nj.deps) {
            std::uint8_t expected =
                static_cast<std::uint8_t>(NodeState::Pending);
            if (nj.state.compare_exchange_strong(
                    expected,
                    static_cast<std::uint8_t>(NodeState::Runnable),
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                enqueue(static_cast<std::uint32_t>(j));
            }
        }
    }
}

void SpeculativeDAG::worker_loop() noexcept {
    const std::size_t total = count_;
    for (;;) {
        std::uint32_t token;
        if (try_dequeue(token)) {
            if (token < static_cast<std::uint32_t>(total)) {
                process_node(token);
            }
            continue;
        }
        if (completed_.load(std::memory_order_acquire) == total) {
            break;
        }
        cpu_relax();
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (active_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            cv_.notify_all();
        }
    }
}

void SpeculativeDAG::run_parallel(std::size_t max_workers) noexcept {
    const std::size_t total = count_;
    if (total == 0) {
        return;
    }
    completed_.store(0, std::memory_order_relaxed);

    for (std::size_t i = 0; i < total; ++i) {
        runnable_or_done(i);
    }

    const std::size_t hw = std::max<std::size_t>(1u,
                                                 std::thread::hardware_concurrency());
    const std::size_t want = (max_workers == 0)
                                 ? (hw > 1 ? hw - 1 : 1)
                                 : std::min(max_workers, hw);
    active_.store(want + 1, std::memory_order_relaxed);

    std::array<std::thread, kMaxNodes> pool{};
    for (std::size_t w = 0; w < want; ++w) {
        pool[w] = std::thread(&SpeculativeDAG::worker_loop, this);
    }
    worker_loop();
    for (std::size_t w = 0; w < want; ++w) {
        if (pool[w].joinable()) {
            pool[w].join();
        }
    }
}

void SpeculativeDAG::run_serial() noexcept {
    const std::size_t total = count_;
    if (total == 0) {
        return;
    }
    std::size_t done = 0;
    while (done < total) {
        bool progressed = false;
        for (std::size_t i = 0; i < total; ++i) {
            Node& n = nodes_[i];
            const std::uint64_t seen =
                n.deps_seen.load(std::memory_order_acquire);
            const std::uint8_t st = n.state.load(std::memory_order_acquire);
            if ((seen & n.deps) == n.deps && !is_done(i) &&
                (st == static_cast<std::uint8_t>(NodeState::Pending) ||
                 st == static_cast<std::uint8_t>(NodeState::Runnable))) {
                process_node(i);
                ++done;
                progressed = true;
            }
        }
        if (!progressed) {
            break;
        }
    }
}

}