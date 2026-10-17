#include "persistence/aof.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <stdexcept>

#include "protocol/resp_parser.hpp"
#include "protocol/resp_writer.hpp"
#include "util/string_utils.hpp"
namespace mr {
ReplayResult decode_aof(std::string_view data, const ReplayCommand& execute) {
  RespParser parser;
  ReplayResult result;
  std::size_t pos = 0, transaction_start = 0;
  bool in_transaction = false;
  std::vector<std::vector<std::string>> pending;
  auto recover = [&](std::size_t offset) { result.recovered_bytes = data.size() - offset; };
  while (pos < data.size()) {
    if (data[pos] != '*')
      throw std::runtime_error("AOF must contain RESP arrays at offset " + std::to_string(pos));
    const auto parsed = parser.parse(std::string_view(data).substr(pos));
    if (parsed.status == ParseStatus::ProtocolError)
      throw std::runtime_error("corrupt AOF at offset " + std::to_string(pos) + ": " +
                               parsed.error);
    if (parsed.status == ParseStatus::Incomplete) {
      recover(in_transaction ? transaction_start : pos);
      return result;
    }
    const auto cmd = upper(parsed.args[0]);
    if (cmd == "MULTI") {
      if (in_transaction || parsed.args.size() != 1) throw std::runtime_error("invalid AOF MULTI");
      in_transaction = true;
      transaction_start = pos;
      pending.clear();
    } else if (cmd == "EXEC") {
      if (!in_transaction || parsed.args.size() != 1) throw std::runtime_error("invalid AOF EXEC");
      for (const auto& args : pending) {
        execute(args);
        ++result.commands;
      }
      pending.clear();
      in_transaction = false;
    } else if (in_transaction)
      pending.push_back(parsed.args);
    else {
      execute(parsed.args);
      ++result.commands;
    }
    pos += parsed.consumed;
  }
  if (in_transaction) recover(transaction_start);
  return result;
}
ReplayResult replay_aof(const std::string& path, const ReplayCommand& execute) {
  const auto data = read_file(path);
  const auto result = decode_aof(data, execute);
  if (result.recovered_bytes) {
    UniqueFd fd(::open(path.c_str(), O_WRONLY | O_CLOEXEC));
    if (!fd || ::ftruncate(fd.get(), static_cast<off_t>(data.size() - result.recovered_bytes)) < 0)
      throw std::runtime_error("cannot truncate AOF tail");
    sync_fd(fd.get());
  }
  return result;
}
std::string encode_records(const std::vector<Record>& records) {
  std::string out;
  for (const auto& r : records) {
    out += encode_command({"SET", r.key, r.value});
    if (r.expire_at_ms)
      out += encode_command({"PEXPIREAT", r.key, std::to_string(*r.expire_at_ms)});
  }
  return out;
}
Aof::Aof(std::string path, FsyncPolicy policy, IClock& clock, Syncer syncer)
    : path_(std::move(path)),
      policy_(policy),
      clock_(clock),
      syncer_(std::move(syncer)),
      last_sync_(clock.now_ms()) {
  open();
  bytes_ = static_cast<std::size_t>(std::filesystem::file_size(path_));
  rewrite_base_ = bytes_;
}
void Aof::open() {
  fd_.reset(::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600));
  if (!fd_) throw std::runtime_error("cannot open AOF " + path_);
}
void Aof::append(const std::string& bytes) {
  if (bytes.empty()) return;
  write_all(fd_.get(), bytes.data(), bytes.size());
  bytes_ += bytes.size();
  dirty_ = true;
  if (policy_ == FsyncPolicy::Always) flush();
}
void Aof::tick() {
  if (policy_ == FsyncPolicy::EverySecond && dirty_ && clock_.now_ms() - last_sync_ >= 1000)
    flush();
}
void Aof::flush() {
  if (dirty_) {
    syncer_(fd_.get());
    dirty_ = false;
    last_sync_ = clock_.now_ms();
  }
}
void Aof::rewrite(const std::vector<Record>& records) {
  const auto data = encode_records(records);
  atomic_file(path_, data);
  open();
  bytes_ = data.size();
  rewrite_base_ = bytes_;
  dirty_ = false;
  last_sync_ = clock_.now_ms();
}
}  // namespace mr
