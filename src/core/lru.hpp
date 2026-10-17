#pragma once
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace mr {
class Lru {
 public:
  void touch(const std::string& key);
  void erase(const std::string& key);
  std::optional<std::string> oldest() const;
  std::optional<std::string> oldest_except(const std::unordered_set<std::string>& keys) const;
  void clear();

 private:
  std::list<std::string> order_;
  std::unordered_map<std::string, std::list<std::string>::iterator> index_;
};
}  // namespace mr
