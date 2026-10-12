#include "gtest/gtest.h"
#include <random>
#include <unordered_map>
#include "engine/order_index_map.hpp"

TEST(OrderIndexMap, InsertFindEraseBasic) {
    OrderIndexMap m(16);
    EXPECT_EQ(-1, m.find(42));
    m.insert(42, 7);
    EXPECT_EQ(7, m.find(42));
    m.insert(42, 9);                 // overwrite
    EXPECT_EQ(9, m.find(42));
    EXPECT_TRUE(m.erase(42));
    EXPECT_EQ(-1, m.find(42));
    EXPECT_FALSE(m.erase(42));       // already gone
}

TEST(OrderIndexMap, ForcedCollisionsProbeAndShift) {
    // Keys that collide: with Fibonacci hashing, consecutive ids land in
    // nearby slots, so a run of ids exercises linear probing and the
    // backward-shift when we erase from the middle of a probe run.
    OrderIndexMap m(16);
    for (OrderId id = 1; id <= 8; ++id) m.insert(id, static_cast<std::int32_t>(id * 10));
    for (OrderId id = 1; id <= 8; ++id) EXPECT_EQ(static_cast<std::int32_t>(id * 10), m.find(id));
    EXPECT_TRUE(m.erase(4));         // remove from the middle
    EXPECT_EQ(-1, m.find(4));
    for (OrderId id : {1u, 2u, 3u, 5u, 6u, 7u, 8u})   // the rest still findable
        EXPECT_EQ(static_cast<std::int32_t>(id * 10), m.find(static_cast<OrderId>(id)));
}

TEST(OrderIndexMap, GrowsPastInitialCapacity) {
    OrderIndexMap m(16);                         // tiny -> forces several rehashes
    for (OrderId id = 1; id <= 1000; ++id) m.insert(id, static_cast<std::int32_t>(id));
    EXPECT_EQ(1000u, m.size());
    for (OrderId id = 1; id <= 1000; ++id) EXPECT_EQ(static_cast<std::int32_t>(id), m.find(id));
    EXPECT_EQ(-1, m.find(1001));
}

// The real test: random insert/erase/find against std::unordered_map, asserting
// identical answers. Backward-shift bugs corrupt the probe chain and show up
// here as a wrong or missing lookup.
TEST(OrderIndexMap, DifferentialAgainstUnorderedMap) {
    std::mt19937_64 rng(2024);
    const std::size_t CAP = 4096;                 // load stays < 50%
    OrderIndexMap flat(CAP);
    std::unordered_map<OrderId, std::int32_t> oracle;
    std::uniform_int_distribution<OrderId> key_dist(1, 2000);   // reused ids -> churn
    std::uniform_int_distribution<int> op_dist(0, 2);
    std::int32_t next_val = 1;

    for (int step = 0; step < 200000; ++step) {
        OrderId k = key_dist(rng);
        switch (op_dist(rng)) {
            case 0: {                              // insert/overwrite
                std::int32_t v = next_val++;
                flat.insert(k, v);
                oracle[k] = v;
                break;
            }
            case 1: {                              // erase
                bool fe = flat.erase(k);
                bool oe = (oracle.erase(k) != 0);
                ASSERT_EQ(oe, fe) << "erase mismatch at step " << step << " key " << k;
                break;
            }
            case 2: {                              // find
                auto it = oracle.find(k);
                std::int32_t expect = (it == oracle.end()) ? -1 : it->second;
                ASSERT_EQ(expect, flat.find(k)) << "find mismatch at step " << step << " key " << k;
                break;
            }
        }
        ASSERT_EQ(oracle.size(), flat.size()) << "size mismatch at step " << step;
    }
}
