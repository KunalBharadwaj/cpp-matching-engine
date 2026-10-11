#include "gtest/gtest.h"
#include <thread>
#include <cstdint>
#include "engine/spsc_ring.hpp"

TEST(SpscRing, FifoFullEmptySingleThread) {
    SpscRing<int> q(4);                 // capacity 4
    int out = -1;
    EXPECT_FALSE(q.try_pop(out));       // empty

    EXPECT_TRUE(q.try_push(10));
    EXPECT_TRUE(q.try_push(20));
    EXPECT_TRUE(q.try_push(30));
    EXPECT_TRUE(q.try_push(40));        // now full (uses all 4 slots)
    EXPECT_FALSE(q.try_push(50));       // full -> refused

    EXPECT_TRUE(q.try_pop(out)); EXPECT_EQ(10, out);   // FIFO order
    EXPECT_TRUE(q.try_pop(out)); EXPECT_EQ(20, out);
    EXPECT_TRUE(q.try_push(50));        // room again
    EXPECT_TRUE(q.try_pop(out)); EXPECT_EQ(30, out);
    EXPECT_TRUE(q.try_pop(out)); EXPECT_EQ(40, out);
    EXPECT_TRUE(q.try_pop(out)); EXPECT_EQ(50, out);
    EXPECT_FALSE(q.try_pop(out));       // empty again
}

// Producer streams 0..N-1 across a thread boundary; consumer must receive every
// value exactly once, in order. A small ring forces real full/empty handoffs
// (spin on refusal), exercising the acquire/release synchronisation. Run under
// ThreadSanitizer (ENABLE_TSAN) to prove it's race-free.
TEST(SpscRing, CrossThreadNothingLostOrReordered) {
    constexpr std::uint64_t N = 2'000'000;
    SpscRing<std::uint64_t> q(1024);

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < N; ++i)
            while (!q.try_push(i)) { /* spin until the consumer drains a slot */ }
    });

    std::uint64_t expected = 0, v = 0;
    bool ordered = true;
    while (expected < N) {
        if (q.try_pop(v)) {
            if (v != expected) { ordered = false; break; }
            ++expected;
        }
    }
    producer.join();

    EXPECT_TRUE(ordered);
    EXPECT_EQ(N, expected);             // every value arrived, exactly once
}
