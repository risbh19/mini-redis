#pragma once
#include <functional>
#include <string>
#include <vector>

#include "core/clock.hpp"
#include "core/store.hpp"
#include "net/socket_util.hpp"
namespace mr {
enum class FsyncPolicy : std::uint8_t { Always, EverySecond, No };
struct ReplayResult {
  std::size_t commands{}, recovered_bytes{};
};
using ReplayCommand = std::function<void(const std::vector<std::string>&)>;
ReplayResult replay_aof(const std::string& path, const ReplayCommand& execute);
ReplayResult decode_aof(std::string_view data, const ReplayCommand& execute);
std::string encode_records(const std::vector<Record>& records);
class Aof {
 public:
  using Syncer = std::function<void(int)>;
  Aof(std::string path, FsyncPolicy policy, IClock& clock, Syncer syncer = sync_fd);
  void append(const std::string& bytes);
  void tick();
  void flush();
  void rewrite(const std::vector<Record>& records);
  std::size_t bytes() const { return bytes_; }
  std::size_t rewrite_base() const { return rewrite_base_; }

 private:
  void open();
  std::string path_;
  FsyncPolicy policy_;
  IClock& clock_;
  Syncer syncer_;
  UniqueFd fd_;
  std::int64_t last_sync_{};
  bool dirty_{};
  std::size_t bytes_{}, rewrite_base_{};
};
}  // namespace mr
