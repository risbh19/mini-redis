#include "core/store.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>

#include "util/string_utils.hpp"
namespace mr {
Entry* Store::live(const std::string& key) {
  auto* entry = table_.find(key);
  if (entry && entry->deadline_ms && *entry->deadline_ms <= clock_.now_ms()) {
    remove(key);
    ++stats_.expired;
    return nullptr;
  }
  return entry;
}
bool Store::remove(const std::string& key) {
  const auto* entry = table_.find(key);
  if (!entry) return false;
  used_ -= cost(key, entry->value);
  table_.erase(key);
  lru_.erase(key);
  expiry_.erase(key);
  dirty_.insert(key);
  return true;
}
std::optional<std::string> Store::get(const std::string& key, bool count_stats) {
  auto* entry = live(key);
  if (!entry) {
    if (count_stats) ++stats_.misses;
    return std::nullopt;
  }
  if (count_stats) ++stats_.hits;
  lru_.touch(key);
  return entry->value;
}
bool Store::exists(const std::string& key) { return live(key) != nullptr; }
bool Store::erase(const std::string& key) { return live(key) && remove(key); }
bool Store::make_room(std::size_t projected,
                      const std::unordered_set<std::string>& protected_keys) {
  if (!limit_ || projected <= limit_) return true;
  if (policy_ == EvictionPolicy::NoEviction) return false;
  while (projected > limit_) {
    const auto victim = lru_.oldest_except(protected_keys);
    if (!victim) return false;
    const auto* value = table_.find(*victim);
    const auto bytes = cost(*victim, value->value);
    remove(*victim);
    projected -= bytes;
    ++stats_.evicted;
  }
  return true;
}
SetResult Store::set(const std::string& key, std::string value, std::optional<std::int64_t> ttl_ms,
                     bool nx, bool xx) {
  const bool present = exists(key);
  if ((nx && present) || (xx && !present)) return SetResult::ConditionFailed;
  std::optional<std::int64_t> deadline;
  if (ttl_ms) {
    std::int64_t when{};
    if (!checked_add(clock_.now_ms(), *ttl_ms, when))
      throw std::overflow_error("expiry out of range");
    deadline = when;
  }
  const auto result = set_many({{key, std::move(value)}});
  if (result == SetResult::Stored && deadline) {
    table_.find(key)->deadline_ms = deadline;
    expiry_.add(key);
  }
  return result;
}
SetResult Store::set_many(const std::vector<std::pair<std::string, std::string>>& pairs) {
  // Last value wins for repeated keys; preflight the whole command before mutating it.
  std::unordered_map<std::string, std::string> values;
  for (const auto& pair : pairs) values.insert_or_assign(pair.first, pair.second);
  std::unordered_set<std::string> protected_keys;
  std::size_t own_cost = 0;
  for (const auto& pair : values) {
    protected_keys.insert(pair.first);
    own_cost += cost(pair.first, pair.second);
    live(pair.first);
  }
  auto projected_usage = [&] {
    std::size_t projected = used_;
    for (const auto& pair : values) {
      if (const auto* e = table_.find(pair.first)) projected -= cost(pair.first, e->value);
      projected += cost(pair.first, pair.second);
    }
    return projected;
  };
  auto projected = projected_usage();
  if (limit_ && projected > limit_) {
    purge_expired();
    projected = projected_usage();
  }
  if (limit_ && own_cost > limit_) return SetResult::OutOfMemory;
  if (!make_room(projected, protected_keys)) return SetResult::OutOfMemory;
  for (auto& pair : values) {
    if (const auto* e = table_.find(pair.first)) used_ -= cost(pair.first, e->value);
    used_ += cost(pair.first, pair.second);
    table_.insert_or_assign(pair.first, Entry{std::move(pair.second), {}});
    expiry_.erase(pair.first);
    lru_.touch(pair.first);
    dirty_.insert(pair.first);
  }
  return SetResult::Stored;
}
bool Store::expire(const std::string& key, std::int64_t ttl_ms) {
  auto* e = live(key);
  if (!e) return false;
  if (ttl_ms <= 0) return remove(key);
  std::int64_t deadline{};
  if (!checked_add(clock_.now_ms(), ttl_ms, deadline))
    throw std::overflow_error("expiry out of range");
  e->deadline_ms = deadline;
  expiry_.add(key);
  dirty_.insert(key);
  return true;
}
bool Store::expire_at(const std::string& key, std::int64_t unix_time) {
  if (unix_time <= clock_.unix_ms()) return erase(key);
  std::int64_t delta{};
  if (__builtin_sub_overflow(unix_time, clock_.unix_ms(), &delta))
    throw std::overflow_error("expiry out of range");
  return expire(key, delta);
}
bool Store::persist(const std::string& key) {
  auto* e = live(key);
  if (!e || !e->deadline_ms) return false;
  e->deadline_ms.reset();
  expiry_.erase(key);
  dirty_.insert(key);
  return true;
}
std::int64_t Store::pttl(const std::string& key) {
  auto* e = live(key);
  if (!e) return -2;
  if (!e->deadline_ms) return -1;
  return *e->deadline_ms - clock_.now_ms();
}
void Store::purge_expired() {
  std::vector<std::string> dead;
  const auto now = clock_.now_ms();
  table_.for_each([&](const auto& k, const auto& v) {
    if (v.deadline_ms && *v.deadline_ms <= now) dead.push_back(k);
  });
  for (const auto& k : dead) {
    remove(k);
    ++stats_.expired;
  }
}
std::size_t Store::run_expiry_cycle(std::size_t max_samples) {
  const auto before = stats_.expired;
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
  std::size_t samples = 0;
  while (samples < max_samples && expiry_.size()) {
    std::size_t expired = 0;
    const auto batch = std::min({std::size_t{20}, expiry_.size(), max_samples - samples});
    for (std::size_t i = 0; i < batch; ++i) {
      const auto k = expiry_.sample(rng_);
      if (k && !live(*k)) ++expired;
    }
    samples += batch;
    if (expired * 4 <= batch || std::chrono::steady_clock::now() >= end) break;
  }
  return static_cast<std::size_t>(stats_.expired - before);
}
std::size_t Store::size() {
  purge_expired();
  return table_.size();
}
std::vector<std::string> Store::keys(std::string_view pattern) {
  purge_expired();
  std::vector<std::string> out;
  table_.for_each([&](const auto& k, const auto&) {
    if (glob_match(pattern, k)) out.push_back(k);
  });
  std::sort(out.begin(), out.end());
  return out;
}
void Store::clear() {
  table_.clear();
  lru_.clear();
  expiry_.clear();
  used_ = 0;
  dirty_.clear();
  flushed_ = true;
}
std::optional<Record> Store::record(const std::string& key) const {
  const auto* e = table_.peek(key);
  if (!e || (e->deadline_ms && *e->deadline_ms <= clock_.now_ms())) return std::nullopt;
  std::optional<std::int64_t> unix_time;
  if (e->deadline_ms) {
    std::int64_t result{};
    if (!checked_add(clock_.unix_ms(), *e->deadline_ms - clock_.now_ms(), result))
      throw std::overflow_error("expiry out of range");
    unix_time = result;
  }
  return Record{key, e->value, unix_time};
}
std::vector<Record> Store::records() const {
  std::vector<Record> out;
  table_.for_each([&](const auto& k, const auto&) {
    if (auto r = record(k)) out.push_back(std::move(*r));
  });
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
  return out;
}
void Store::restore(const std::vector<Record>& records) {
  clear();
  const auto saved_limit = limit_;
  limit_ = 0;
  for (const auto& r : records) {
    if (r.expire_at_ms && *r.expire_at_ms <= clock_.unix_ms()) continue;
    set(r.key, r.value);
    if (r.expire_at_ms) expire_at(r.key, *r.expire_at_ms);
  }
  limit_ = saved_limit;
  dirty_.clear();
  flushed_ = false;
}
std::vector<std::string> Store::take_dirty() {
  std::vector<std::string> out(dirty_.begin(), dirty_.end());
  dirty_.clear();
  std::sort(out.begin(), out.end());
  return out;
}
}  // namespace mr
