#pragma once
#include <vector>
#include <cstddef>
#include <cstdint>
#include <cassert>
#include "engine/types.hpp"

// Open-addressing (linear-probing) hash map: OrderId -> pool index. This is the
// registry behind O(1) cancel/modify, replacing std::unordered_map.
//
// Why: std::unordered_map chains buckets with a heap node per entry and a
// pointer chase per lookup -- cache-hostile. Here every entry lives in one
// contiguous array, so a lookup is a short scan of adjacent slots that usually
// stays within a cache line or two.
//
// Deletion uses BACKWARD-SHIFT (Knuth 6.4 alg. R), not tombstones: on erase we
// pull later entries back into the hole so probe runs remain contiguous. The
// table never fills with dead slots, so -- unlike a tombstone table -- deletes
// never force a rehash and probe lengths stay bounded for the life of the
// engine, which matters for the tail. Growth is the only thing that rehashes:
// the table doubles when it would pass a 0.5 load factor (so find() always
// terminates at an empty slot, and probe runs stay short).
//
// Capacity is a power of two; Fibonacci hashing spreads the mostly-sequential
// ids across the table.
class OrderIndexMap {
    static constexpr std::int32_t EMPTY = -1;
    static constexpr std::uint64_t GOLDEN = 0x9E3779B97F4A7C15ull;

    struct Slot {
        OrderId key{};
        std::int32_t val{EMPTY};        // EMPTY (-1) marks a free slot
    };

    std::vector<Slot> slots_;
    std::size_t mask_{0};
    std::size_t shift_{0};              // 64 - log2(capacity), for Fibonacci hashing
    std::size_t count_{0};

    static std::size_t next_pow2(std::size_t x) {
        std::size_t p = 1; while (p < x) p <<= 1; return p;
    }
    std::size_t home(OrderId k) const {
        return (static_cast<std::uint64_t>(k) * GOLDEN) >> shift_;
    }

    void set_capacity(std::size_t cap) {            // cap must be a power of two
        mask_ = cap - 1;
        shift_ = 64;
        for (std::size_t c = cap; c > 1; c >>= 1) --shift_;   // shift_ = 64 - log2(cap)
    }

    // Insert without the load-factor check (used during rehash and by insert()).
    void place(OrderId key, std::int32_t val) {
        std::size_t i = home(key);
        while (slots_[i].val != EMPTY) {
            if (slots_[i].key == key) { slots_[i].val = val; return; }   // overwrite
            i = (i + 1) & mask_;
        }
        slots_[i].key = key;
        slots_[i].val = val;
        ++count_;
    }

    void rehash(std::size_t new_cap) {
        std::vector<Slot> old = std::move(slots_);
        std::size_t cap = next_pow2(new_cap);
        slots_.assign(cap, Slot{});
        set_capacity(cap);
        count_ = 0;
        for (const Slot& s : old) if (s.val != EMPTY) place(s.key, s.val);
    }

public:
    explicit OrderIndexMap(std::size_t min_capacity) {
        std::size_t cap = next_pow2(min_capacity < 16 ? 16 : min_capacity);
        slots_.assign(cap, Slot{});
        set_capacity(cap);
    }

    std::size_t size() const { return count_; }

    // Insert or overwrite key -> val; doubles the table before it passes 0.5 load.
    void insert(OrderId key, std::int32_t val) {
        if ((count_ + 1) * 2 >= slots_.size()) rehash(slots_.size() * 2);
        place(key, val);
    }

    // Returns the stored index, or -1 if absent.
    std::int32_t find(OrderId key) const {
        std::size_t i = home(key);
        while (slots_[i].val != EMPTY) {
            if (slots_[i].key == key) return slots_[i].val;
            i = (i + 1) & mask_;
        }
        return EMPTY;
    }

    // Returns true if the key was present and removed.
    bool erase(OrderId key) {
        std::size_t i = home(key);
        while (slots_[i].val != EMPTY && slots_[i].key != key) i = (i + 1) & mask_;
        if (slots_[i].val == EMPTY) return false;               // absent

        --count_;
        // Backward-shift: close the hole at i by pulling in any later element
        // whose home is not cyclically inside (i, j].
        std::size_t j = i;
        for (;;) {
            slots_[i].val = EMPTY;
            for (;;) {
                j = (j + 1) & mask_;
                if (slots_[j].val == EMPTY) return true;        // probe run ended
                std::size_t k = home(slots_[j].key);
                bool in_range = (i <= j) ? (i < k && k <= j) : (k <= j || k > i);
                if (!in_range) break;                           // j may fill the hole at i
            }
            slots_[i] = slots_[j];
            i = j;
        }
    }
};
