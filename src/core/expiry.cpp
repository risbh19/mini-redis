#include "core/expiry.hpp"
namespace mr {
void ExpiryIndex::add(const std::string& key) {
  if (positions_.count(key) == 0) {
    positions_[key] = keys_.size();
    keys_.push_back(key);
  }
}
void ExpiryIndex::erase(const std::string& key) {
  const auto it = positions_.find(key);
  if (it == positions_.end()) return;
  const auto pos = it->second;
  if (pos + 1 != keys_.size()) {
    keys_[pos] = std::move(keys_.back());
    positions_[keys_[pos]] = pos;
  }
  keys_.pop_back();
  positions_.erase(it);
}
std::optional<std::string> ExpiryIndex::sample(std::mt19937_64& rng) const {
  if (keys_.empty()) return std::nullopt;
  return keys_[std::uniform_int_distribution<std::size_t>(0, keys_.size() - 1)(rng)];
}
}  // namespace mr
