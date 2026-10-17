#include "core/lru.hpp"
namespace mr {
void Lru::touch(const std::string& key) {
  const auto it = index_.find(key);
  if (it != index_.end())
    order_.splice(order_.begin(), order_, it->second);
  else {
    order_.push_front(key);
    index_.emplace(key, order_.begin());
  }
}
void Lru::erase(const std::string& key) {
  const auto it = index_.find(key);
  if (it != index_.end()) {
    order_.erase(it->second);
    index_.erase(it);
  }
}
std::optional<std::string> Lru::oldest() const {
  if (order_.empty()) return std::nullopt;
  return order_.back();
}
void Lru::clear() {
  index_.clear();
  order_.clear();
}
std::optional<std::string> Lru::oldest_except(const std::unordered_set<std::string>& keys) const {
  for (auto it = order_.rbegin(); it != order_.rend(); ++it)
    if (keys.count(*it) == 0) return *it;
  return std::nullopt;
}
}  // namespace mr
