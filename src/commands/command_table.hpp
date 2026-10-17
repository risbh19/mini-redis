#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "core/store.hpp"
#include "persistence/aof.hpp"
#include "protocol/reply.hpp"
namespace mr {
class CommandTable {
 public:
  CommandTable(Config config, IClock& clock);
  ~CommandTable();
  Reply execute(const std::vector<std::string>& args);
  void tick();
  void flush();
  bool shutdown_requested() const { return shutdown_.load(); }
  std::atomic<std::size_t> clients{0};

 private:
  Reply dispatch(const std::vector<std::string>& args, bool replay = false);
  void journal_changes();
  std::string path(const std::string& filename) const;
  Config config_;
  IClock& clock_;
  Store store_;
  std::mutex mutex_;
  std::unique_ptr<Aof> aof_;
  UniqueFd directory_lock_;
  std::atomic<bool> shutdown_{false};
  std::uint64_t commands_{};
  std::int64_t started_;
  bool persistence_failed_{};
};
}  // namespace mr
