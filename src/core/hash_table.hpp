#pragma once
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mr {
// Separate chaining; at most two nodes (including empty buckets) migrate per operation.
// All references/visitors must be discarded before the next mutating operation.
template <class T>
class HashTable {
  struct Node {
    std::string key;
    T value;
    std::unique_ptr<Node> next;
    Node(std::string k, T v) : key(std::move(k)), value(std::move(v)) {}
  };
  using Buckets = std::vector<std::unique_ptr<Node>>;

 public:
  HashTable() : buckets_(8) {}
  ~HashTable() { clear(); }
  HashTable(const HashTable&) = delete;
  HashTable& operator=(const HashTable&) = delete;
  static std::uint64_t hash(std::string_view key) {
    std::uint64_t h = 14695981039346656037ULL;
    for (const char c : key) {
      h ^= static_cast<unsigned char>(c);
      h *= 1099511628211ULL;
    }
    return h;
  }
  std::size_t size() const { return size_; }
  bool rehashing() const { return !next_.empty(); }
  std::size_t bucket_count() const { return buckets_.size() + next_.size(); }
  T* find(std::string_view key) {
    migrate();
    if (auto* n = locate(buckets_, key)) return &n->value;
    if (auto* n = locate(next_, key)) return &n->value;
    return nullptr;
  }
  const T* peek(std::string_view key) const {
    if (auto* n = locate(buckets_, key)) return &n->value;
    if (auto* n = locate(next_, key)) return &n->value;
    return nullptr;
  }
  bool insert_or_assign(std::string key, T value) {
    if (auto* old = find(key)) {
      *old = std::move(value);
      return false;
    }
    if (next_.empty() && size_ >= buckets_.size()) next_.resize(buckets_.size() * 2);
    auto& dest = next_.empty() ? buckets_ : next_;
    const auto i = index(key, dest.size());
    auto node = std::make_unique<Node>(std::move(key), std::move(value));
    node->next = std::move(dest[i]);
    dest[i] = std::move(node);
    ++size_;
    return true;
  }
  bool erase(std::string_view key) {
    migrate();
    for (auto* table : {&buckets_, &next_}) {
      if (table->empty()) continue;
      auto* link = &(*table)[index(key, table->size())];
      while (*link) {
        if ((*link)->key == key) {
          auto dead = std::move(*link);
          *link = std::move(dead->next);
          --size_;
          if (next_.empty() && buckets_.size() > 8 && size_ < buckets_.size() / 10)
            next_.resize(buckets_.size() / 2);
          return true;
        }
        link = &(*link)->next;
      }
    }
    return false;
  }
  template <class F>
  void for_each(F&& fn) const {
    for (const auto* table : {&buckets_, &next_})
      for (const auto& head : *table)
        for (auto* n = head.get(); n; n = n->next.get()) fn(n->key, n->value);
  }
  template <class Generator>
  std::string random_key(Generator& rng) const {
    std::string result;
    std::size_t seen = 0;
    for_each([&](const auto& key, const auto&) {
      if (std::uniform_int_distribution<std::size_t>(0, seen++)(rng) == 0) result = key;
    });
    return result;  // Reservoir sampling, O(N); expiry has its own O(1) index.
  }
  void clear() {
    // Iterative destruction avoids stack overflow for a long collision chain.
    for (auto* table : {&buckets_, &next_})
      for (auto& head : *table)
        while (head) {
          auto node = std::move(head);
          head = std::move(node->next);
        }
    next_.clear();
    buckets_.resize(8);
    cursor_ = 0;
    size_ = 0;
  }

 private:
  static std::size_t index(std::string_view key, std::size_t n) {
    return static_cast<std::size_t>(hash(key)) & (n - 1);
  }
  static Node* locate(const Buckets& table, std::string_view key) {
    if (table.empty()) return nullptr;
    for (auto* n = table[index(key, table.size())].get(); n; n = n->next.get())
      if (n->key == key) return n;
    return nullptr;
  }
  void migrate() {
    for (int work = 0; work < 2 && !next_.empty(); ++work) {
      if (cursor_ == buckets_.size()) {
        buckets_ = std::move(next_);
        next_.clear();
        cursor_ = 0;
        break;
      }
      auto& head = buckets_[cursor_];
      if (!head) {
        ++cursor_;
        continue;
      }
      auto node = std::move(head);
      head = std::move(node->next);
      auto& dest = next_[index(node->key, next_.size())];
      node->next = std::move(dest);
      dest = std::move(node);
    }
    // A shrink can finish after the last erase. Continue reducing sparse tables
    // during subsequent lookups instead of requiring another deletion.
    if (next_.empty() && buckets_.size() > 8 && size_ < buckets_.size() / 10)
      next_.resize(buckets_.size() / 2);
  }
  Buckets buckets_, next_;
  std::size_t cursor_{}, size_{};
};
}  // namespace mr
