#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

#include "core/hash_table.hpp"
#include "core/store.hpp"
TEST(HashTable, DifferentialRandomizedOracle) {
  mr::HashTable<std::string> table;
  std::unordered_map<std::string, std::string> oracle;
  std::mt19937 rng(19);
  for (int i = 0; i < 100000; ++i) {
    const auto key = std::to_string(rng() % 2000);
    const auto value = std::to_string(rng());
    switch (rng() % 3) {
      case 0:
        table.insert_or_assign(key, value);
        oracle[key] = value;
        break;
      case 1:
        EXPECT_EQ(table.erase(key), oracle.erase(key) != 0);
        break;
      default: {
        const auto* got = table.find(key);
        const auto found = oracle.find(key);
        ASSERT_EQ(got != nullptr, found != oracle.end());
        if (got) {
          EXPECT_EQ(*got, found->second);
        }
      }
    }
    ASSERT_EQ(table.size(), oracle.size());
  }
  std::size_t seen = 0;
  table.for_each([&](const auto& k, const auto& v) {
    EXPECT_EQ(oracle.at(k), v);
    ++seen;
  });
  EXPECT_EQ(seen, oracle.size());
}
TEST(HashTable, ResizeAndShrinkPreserveAllKeys) {
  mr::HashTable<int> table;
  for (int i = 0; i < 10000; ++i) table.insert_or_assign(std::to_string(i), i);
  for (int i = 0; i < 9900; ++i) ASSERT_TRUE(table.erase(std::to_string(i)));
  for (int round = 0; round < 1000; ++round)
    for (int i = 9900; i < 10000; ++i) ASSERT_EQ(*table.find(std::to_string(i)), i);
  EXPECT_LE(table.bucket_count(), 2048U);
  table.clear();
  EXPECT_EQ(table.size(), 0U);
}
TEST(HashTable, SamplesExistingKeysDuringRehash) {
  mr::HashTable<int> table;
  std::mt19937_64 rng(42);
  for (int i = 0; i < 10; ++i) table.insert_or_assign(std::to_string(i), i);
  for (int i = 0; i < 20; ++i) EXPECT_NE(table.find(table.random_key(rng)), nullptr);
}
class StoreTest : public testing::Test {
 protected:
  mr::FakeClock clock;
  mr::Store store{clock};
};
TEST_F(StoreTest, BasicsAndLazyExpiry) {
  EXPECT_EQ(store.set("a", "b", 100), mr::SetResult::Stored);
  clock.advance(99);
  EXPECT_EQ(store.get("a"), "b");
  clock.advance(1);
  EXPECT_FALSE(store.exists("a"));
  EXPECT_EQ(store.pttl("a"), -2);
  EXPECT_EQ(store.stats().expired, 1U);
}
TEST_F(StoreTest, PersistAndOverwriteClearExpiry) {
  store.set("a", "b", 100);
  EXPECT_TRUE(store.persist("a"));
  EXPECT_FALSE(store.persist("a"));
  EXPECT_EQ(store.pttl("a"), -1);
  store.expire("a", 100);
  store.set("a", "c");
  clock.advance(1000);
  EXPECT_EQ(store.get("a"), "c");
}
TEST_F(StoreTest, SetConditions) {
  EXPECT_EQ(store.set("a", "b", {}, false, true), mr::SetResult::ConditionFailed);
  store.set("a", "b");
  EXPECT_EQ(store.set("a", "c", {}, true), mr::SetResult::ConditionFailed);
  EXPECT_EQ(store.get("a"), "b");
}
TEST_F(StoreTest, ActiveExpiryWithoutAccess) {
  for (int i = 0; i < 100; ++i) store.set(std::to_string(i), "v", 10);
  clock.advance(10);
  for (int i = 0; i < 10; ++i) store.run_expiry_cycle();
  EXPECT_EQ(store.used_memory(), 0U);
  EXPECT_EQ(store.stats().expired, 100U);
}
TEST_F(StoreTest, SteadyClockIgnoresWallJumps) {
  store.set("a", "b", 100);
  clock.jump_wall(100000);
  EXPECT_EQ(store.pttl("a"), 100);
  clock.advance(100);
  EXPECT_FALSE(store.get("a"));
}
TEST_F(StoreTest, ExpireZeroAndNegativeDeletes) {
  store.set("a", "b");
  EXPECT_TRUE(store.expire("a", 0));
  EXPECT_FALSE(store.expire("a", 100));
  store.set("a", "b");
  EXPECT_TRUE(store.expire("a", -1));
  EXPECT_EQ(store.size(), 0U);
}
TEST_F(StoreTest, LruTouchOnRead) {
  store.set_memory_limit(324, mr::EvictionPolicy::AllKeysLru);
  store.set("a", "1");
  store.set("b", "2");
  store.get("a");
  EXPECT_EQ(store.set("c", "3"), mr::SetResult::Stored);
  EXPECT_FALSE(store.exists("b"));
  EXPECT_TRUE(store.exists("a"));
  EXPECT_EQ(store.stats().evicted, 1U);
}
TEST_F(StoreTest, NoEvictionFailureDoesNotMutate) {
  store.set_memory_limit(162, mr::EvictionPolicy::NoEviction);
  store.set("a", "1");
  EXPECT_EQ(store.set("b", "2"), mr::SetResult::OutOfMemory);
  EXPECT_EQ(store.get("a"), "1");
  EXPECT_EQ(store.size(), 1U);
}
TEST_F(StoreTest, ExpiredEntriesReclaimedBeforeLruVictims) {
  store.set_memory_limit(324, mr::EvictionPolicy::AllKeysLru);
  store.set("a", "1");
  store.set("b", "2", 10);
  clock.advance(10);
  store.set("c", "3");
  EXPECT_TRUE(store.exists("a"));
  EXPECT_EQ(store.stats().evicted, 0U);
  EXPECT_EQ(store.stats().expired, 1U);
}
TEST_F(StoreTest, MsetAtomicOomAndDuplicateKeys) {
  store.set_memory_limit(324, mr::EvictionPolicy::AllKeysLru);
  store.set("a", "1");
  EXPECT_EQ(store.set_many({{"a", "2"}, {"b", "2"}, {"c", "3"}}), mr::SetResult::OutOfMemory);
  EXPECT_EQ(store.get("a"), "1");
  EXPECT_FALSE(store.exists("b"));
  EXPECT_EQ(store.set_many({{"a", "2"}, {"a", "3"}}), mr::SetResult::Stored);
  EXPECT_EQ(store.get("a"), "3");
}
TEST_F(StoreTest, ImpossibleLargeWriteDoesNotEvict) {
  store.set_memory_limit(324, mr::EvictionPolicy::AllKeysLru);
  store.set("a", "1");
  EXPECT_EQ(store.set("b", std::string(400, 'x')), mr::SetResult::OutOfMemory);
  EXPECT_EQ(store.get("a"), "1");
}
TEST_F(StoreTest, RestoreSkipsExpiredAndPreservesLiveDeadline) {
  const auto t = clock.unix_ms();
  store.restore({{"a", "1", t - 1}, {"b", "2", t + 100}, {"c", "3", {}}});
  EXPECT_EQ(store.size(), 2U);
  EXPECT_EQ(store.pttl("b"), 100);
  EXPECT_EQ(store.pttl("c"), -1);
}
TEST_F(StoreTest, DifferentialRandomStoreModel) {
  std::unordered_map<std::string, std::pair<std::string, std::int64_t>> oracle;
  std::mt19937 rng(321);
  for (int i = 0; i < 20000; ++i) {
    clock.advance(1);
    const auto key = std::to_string(rng() % 100);
    for (auto it = oracle.begin(); it != oracle.end();) {
      if (it->second.second && it->second.second <= clock.now_ms())
        it = oracle.erase(it);
      else
        ++it;
    }
    const auto op = rng() % 3;
    if (op == 0) {
      const auto value = std::to_string(rng());
      const std::int64_t ttl = static_cast<std::int64_t>(rng() % 100 + 1);
      store.set(key, value, ttl);
      oracle[key] = {value, clock.now_ms() + ttl};
    } else if (op == 1)
      EXPECT_EQ(store.erase(key), oracle.erase(key) != 0);
    else {
      const auto got = store.get(key);
      EXPECT_EQ(got.has_value(), oracle.count(key) != 0);
      if (got) {
        EXPECT_EQ(*got, oracle.at(key).first);
      }
    }
    EXPECT_EQ(store.size(), oracle.size());
  }
}
