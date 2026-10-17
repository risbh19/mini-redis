#pragma once
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
namespace mr {
class ExpiryIndex {
 public:
  void add(const std::string& key);
  void erase(const std::string& key);
  std::optional<std::string> sample(std::mt19937_64& rng) const;
  std::size_t size() const { return keys_.size(); }
  void clear() {
    keys_.clear();
    positions_.clear();
  }

 private:
  std::vector<std::string> keys_;
  std::unordered_map<std::string, std::size_t> positions_;
};
}  // namespace mr
