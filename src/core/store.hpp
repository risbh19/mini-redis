#pragma once
#include <optional>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/clock.hpp"
#include "core/expiry.hpp"
#include "core/hash_table.hpp"
#include "core/lru.hpp"
namespace mr {
struct Entry {
  std::string value;
  std::optional<std::int64_t> deadline_ms;
};
struct Record {
  std::string key, value;
  std::optional<std::int64_t> expire_at_ms;
};
enum class EvictionPolicy : std::uint8_t { NoEviction, AllKeysLru };
enum class SetResult : std::uint8_t { Stored, ConditionFailed, OutOfMemory };
struct StoreStats {
  std::uint64_t hits{}, misses{}, expired{}, evicted{};
};
// Not internally synchronized. CommandTable owns the command/persistence transaction lock.
class Store {
 public:
  explicit Store(IClock& clock) : clock_(clock) {}
  std::optional<std::string> get(const std::string& key, bool count_stats = true);
  bool exists(const std::string& key);
  SetResult set(const std::string& key, std::string value, std::optional<std::int64_t> ttl_ms = {},
                bool nx = false, bool xx = false);
  SetResult set_many(const std::vector<std::pair<std::string, std::string>>& pairs);
  bool erase(const std::string& key);
  bool expire(const std::string& key, std::int64_t ttl_ms);
  bool expire_at(const std::string& key, std::int64_t unix_ms);
  bool persist(const std::string& key);
  std::int64_t pttl(const std::string& key);
  std::size_t size();
  std::vector<std::string> keys(std::string_view pattern);
  void clear();
  std::size_t run_expiry_cycle(std::size_t max_samples = 1000);
  void purge_expired();
  void set_memory_limit(std::size_t bytes, EvictionPolicy policy) {
    limit_ = bytes;
    policy_ = policy;
  }
  std::size_t used_memory() const { return used_; }
  std::size_t memory_limit() const { return limit_; }
  EvictionPolicy policy() const { return policy_; }
  const StoreStats& stats() const { return stats_; }
  std::vector<Record> records() const;
  void restore(const std::vector<Record>& records);
  std::vector<std::string> take_dirty();
  bool take_flushed() {
    const bool old = flushed_;
    flushed_ = false;
    return old;
  }
  std::optional<Record> record(const std::string& key) const;
  static std::size_t cost(std::string_view key, std::string_view value) {
    return key.size() + value.size() + 160;
  }

 private:
  Entry* live(const std::string& key);
  bool remove(const std::string& key);
  bool make_room(std::size_t projected, const std::unordered_set<std::string>& protected_keys);
  IClock& clock_;
  HashTable<Entry> table_;
  Lru lru_;
  ExpiryIndex expiry_;
  std::mt19937_64 rng_{0x4D524442};
  std::size_t used_{}, limit_{};
  EvictionPolicy policy_{EvictionPolicy::NoEviction};
  StoreStats stats_;
  std::unordered_set<std::string> dirty_;
  bool flushed_{};
};
}  // namespace mr
