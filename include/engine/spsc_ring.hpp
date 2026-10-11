#pragma once
#include <atomic>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <cassert>

// Single-producer / single-consumer lock-free ring buffer. One thread calls
// try_push, one other calls try_pop -- no locks, no CAS, just acquire/release
// on two counters. This is the seam that lets an ingress thread hand orders to
// the matching thread without either blocking.
//
// Design:
//  * head_/tail_ are monotonically increasing 64-bit counters; we mask only
//    when indexing. tail_ - head_ gives the exact fill level, so full and empty
//    are distinguishable without sacrificing a slot, and the counters won't
//    wrap in any realistic run (2^64 ops).
//  * Ordering: the producer writes buf_[slot] then release-stores tail_; the
//    consumer acquire-loads tail_, so observing the new tail_ guarantees the
//    slot write is visible. Symmetric on head_ for slot reuse. Each thread
//    loads the counter it owns with relaxed (it's the only writer).
//  * head_ and tail_ sit on separate cache lines (alignas 64) so the
//    producer's tail_ stores don't invalidate the consumer's head_ line --
//    false sharing here would quietly cost most of the throughput.
template <class T>
class SpscRing {
    static constexpr std::size_t CACHELINE = 64;

    std::vector<T> buf_;
    std::size_t mask_;

    alignas(CACHELINE) std::atomic<std::uint64_t> head_{0};  // owned by consumer
    alignas(CACHELINE) std::atomic<std::uint64_t> tail_{0};  // owned by producer
    char pad_[CACHELINE];                                    // isolate tail_'s line

    static bool is_pow2(std::size_t x) { return x && (x & (x - 1)) == 0; }

public:
    // capacity must be a power of two.
    explicit SpscRing(std::size_t capacity)
        : buf_(capacity), mask_(capacity - 1) {
        assert(is_pow2(capacity) && "SpscRing capacity must be a power of two");
        (void)pad_;
    }

    // Producer side. Returns false (without blocking) if the ring is full.
    bool try_push(const T& v) {
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_.load(std::memory_order_acquire) == buf_.size())
            return false;                                   // full
        buf_[t & mask_] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false (without blocking) if the ring is empty.
    bool try_pop(T& out) {
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire))
            return false;                                   // empty
        out = buf_[h & mask_];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Approximate fill level (exact when called from the consumer).
    std::size_t size_approx() const {
        return static_cast<std::size_t>(tail_.load(std::memory_order_acquire) -
                                        head_.load(std::memory_order_acquire));
    }
    std::size_t capacity() const { return buf_.size(); }
};
