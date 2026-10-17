#include "commands/command_table.hpp"

#include <fcntl.h>
#include <sys/file.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "persistence/snapshot.hpp"
#include "protocol/resp_writer.hpp"
#include "util/logger.hpp"
#include "util/string_utils.hpp"
namespace mr {
namespace {
struct Spec {
  int arity;
  bool write;
};
const std::unordered_map<std::string, Spec> specs{
    {"PING", {-1, false}},        {"ECHO", {2, false}},     {"SET", {-3, true}},
    {"GET", {2, false}},          {"DEL", {-2, true}},      {"EXISTS", {-2, false}},
    {"EXPIRE", {3, true}},        {"PEXPIRE", {3, true}},   {"PEXPIREAT", {3, true}},
    {"TTL", {2, false}},          {"PTTL", {2, false}},     {"PERSIST", {2, true}},
    {"INCR", {2, true}},          {"DECR", {2, true}},      {"INCRBY", {3, true}},
    {"DECRBY", {3, true}},        {"APPEND", {3, true}},    {"STRLEN", {2, false}},
    {"GETSET", {3, true}},        {"MSET", {-3, true}},     {"MGET", {-2, false}},
    {"KEYS", {2, false}},         {"DBSIZE", {1, false}},   {"FLUSHALL", {1, true}},
    {"INFO", {-1, false}},        {"CONFIG", {-2, false}},  {"SAVE", {1, false}},
    {"BGREWRITEAOF", {1, false}}, {"SHUTDOWN", {1, false}}, {"QUIT", {1, false}},
    {"EXIT", {1, false}},         {"COMMAND", {-1, false}}, {"SELECT", {2, false}},
    {"CLIENT", {-2, false}}};
Reply integer_error() { return Reply::error("ERR value is not an integer or out of range"); }
Reply oom() { return Reply::error("OOM command not allowed when used memory > 'maxmemory'"); }
Reply set_reply(SetResult r) {
  return r == SetResult::Stored            ? Reply::ok()
         : r == SetResult::ConditionFailed ? Reply::null()
                                           : oom();
}
}  // namespace
std::string CommandTable::path(const std::string& filename) const {
  return (std::filesystem::path(config_.dir) / filename).string();
}
CommandTable::CommandTable(Config config, IClock& clock)
    : config_(std::move(config)), clock_(clock), store_(clock), started_(clock.now_ms()) {
  std::filesystem::create_directories(config_.dir);
  directory_lock_.reset(
      ::open(path("mini-redis.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
  if (!directory_lock_ || ::flock(directory_lock_.get(), LOCK_EX | LOCK_NB) < 0)
    throw std::runtime_error("data directory already in use or cannot be locked");
  const bool has_aof = std::filesystem::exists(path("appendonly.aof"));
  if (config_.appendonly && has_aof) {
    const auto result = replay_aof(path("appendonly.aof"), [&](const auto& args) {
      const auto name = args.empty() ? "" : upper(args[0]);
      if (name != "SET" && name != "DEL" && name != "PEXPIREAT" && name != "FLUSHALL")
        throw std::runtime_error("unsupported AOF command " + safe_line(name));
      const auto reply = dispatch(args, true);
      if (reply.is_error())
        throw std::runtime_error("AOF command failed: " + std::get<Error>(reply.value).value);
      store_.take_dirty();
      store_.take_flushed();
    });
    if (result.recovered_bytes)
      log("warn",
          "recovered truncated AOF tail: " + std::to_string(result.recovered_bytes) + " bytes");
  } else if (std::filesystem::exists(path("dump.mrdb")))
    load_snapshot(path("dump.mrdb"), store_);
  if (config_.appendonly) {
    // Seed a new AOF from the loaded snapshot before accepting any client writes.
    if (!has_aof) atomic_file(path("appendonly.aof"), encode_records(store_.records()));
    aof_ = std::make_unique<Aof>(path("appendonly.aof"), config_.appendfsync, clock_);
  }
  store_.set_memory_limit(config_.maxmemory, config_.memory_policy);
  store_.take_dirty();
  store_.take_flushed();
}
CommandTable::~CommandTable() {
  try {
    flush();
  } catch (const std::exception& e) {
    log("error", e.what());
  }
}
Reply CommandTable::execute(const std::vector<std::string>& args) {
  const std::lock_guard<std::mutex> lock(mutex_);
  ++commands_;
  try {
    if (persistence_failed_ && !args.empty()) {
      const auto it = specs.find(upper(args[0]));
      if (it != specs.end() && it->second.write)
        return Reply::error("MISCONF persistence failed; writes disabled until restart");
    }
    auto reply = dispatch(args);
    journal_changes();
    return reply;
  } catch (const std::overflow_error&) {
    return integer_error();
  } catch (const std::exception& e) {
    log("error", e.what());
    return Reply::error("ERR " + safe_line(e.what()));
  }
}
void CommandTable::journal_changes() {
  const bool flushed = store_.take_flushed();
  const auto dirty = store_.take_dirty();
  if (!aof_ || persistence_failed_) return;
  std::string bytes;
  if (flushed) bytes += encode_command({"FLUSHALL"});
  for (const auto& key : dirty) {
    if (auto r = store_.record(key))
      bytes += encode_records({*r});
    else
      bytes += encode_command({"DEL", key});
  }
  if (bytes.empty()) return;
  try {
    aof_->append(encode_command({"MULTI"}) + bytes + encode_command({"EXEC"}));
  } catch (...) {
    persistence_failed_ = true;
    throw;
  }
}
void CommandTable::tick() {
  const std::lock_guard<std::mutex> lock(mutex_);
  try {
    store_.run_expiry_cycle();
    journal_changes();
    if (aof_ && !persistence_failed_) {
      aof_->tick();
      if (config_.aof_rewrite_min && aof_->bytes() >= config_.aof_rewrite_min &&
          aof_->bytes() / 2 > aof_->rewrite_base())
        aof_->rewrite(store_.records());
    }
  } catch (const std::exception& e) {
    persistence_failed_ = true;
    log("error", e.what());
  }
}
void CommandTable::flush() {
  const std::lock_guard<std::mutex> lock(mutex_);
  try {
    if (aof_) aof_->flush();
  } catch (...) {
    persistence_failed_ = true;
    throw;
  }
}
Reply CommandTable::dispatch(const std::vector<std::string>& a, bool replay) {
  if (a.empty()) return Reply::ok();
  const auto cmd = upper(a[0]);
  const auto spec = specs.find(cmd);
  if (spec == specs.end()) return Reply::error("ERR unknown command '" + safe_line(a[0]) + "'");
  const auto n = static_cast<int>(a.size());
  const int arity = spec->second.arity;
  if ((arity > 0 && n != arity) || (arity < 0 && n < -arity) || (cmd == "PING" && n > 2))
    return Reply::error("ERR wrong number of arguments for '" + lower(cmd) + "' command");
  if (cmd == "PING") return n == 1 ? Reply::simple("PONG") : Reply::bulk(a[1]);
  if (cmd == "ECHO") return Reply::bulk(a[1]);
  if (cmd == "COMMAND") return Reply::array({});
  if (cmd == "CLIENT") {
    if (upper(a[1]) == "SETINFO" && n == 4) return Reply::ok();
    if (upper(a[1]) == "SETNAME" && n == 3) return Reply::ok();
    return Reply::error("ERR unsupported CLIENT subcommand");
  }
  if (cmd == "SELECT")
    return a[1] == "0" ? Reply::ok() : Reply::error("ERR DB index is out of range");
  if (cmd == "QUIT" || cmd == "EXIT") return Reply::ok();
  if (cmd == "SET") {
    bool nx = false, xx = false;
    std::optional<std::int64_t> ttl;
    for (std::size_t i = 3; i < a.size(); ++i) {
      const auto option = upper(a[i]);
      if (option == "NX" && !nx && !xx)
        nx = true;
      else if (option == "XX" && !nx && !xx)
        xx = true;
      else if ((option == "EX" || option == "PX") && !ttl && i + 1 < a.size()) {
        const auto value = parse_i64(a[++i]);
        if (!value) return integer_error();
        std::int64_t milliseconds{};
        if (*value <= 0 || !checked_mul(*value, option == "EX" ? 1000 : 1, milliseconds))
          return Reply::error("ERR invalid expire time in 'set' command");
        std::int64_t absolute{};
        if (!checked_add(clock_.unix_ms(), milliseconds, absolute))
          return Reply::error("ERR invalid expire time in 'set' command");
        ttl = milliseconds;
      } else
        return Reply::error("ERR syntax error");
    }
    return set_reply(store_.set(a[1], a[2], ttl, nx, xx));
  }
  if (cmd == "GET" || cmd == "STRLEN") {
    const auto value = store_.get(a[1]);
    if (cmd == "STRLEN")
      return Reply::integer(value ? static_cast<std::int64_t>(value->size()) : 0);
    return value ? Reply::bulk(*value) : Reply::null();
  }
  if (cmd == "DEL" || cmd == "EXISTS") {
    std::int64_t count = 0;
    for (std::size_t i = 1; i < a.size(); ++i)
      if (cmd == "DEL" ? store_.erase(a[i]) : store_.exists(a[i])) ++count;
    return Reply::integer(count);
  }
  if (cmd == "TTL" || cmd == "PTTL") {
    const auto ttl = store_.pttl(a[1]);
    return Reply::integer(cmd == "PTTL" || ttl < 0 ? ttl
                                                   : ttl / 1000 + (ttl % 1000 >= 500 ? 1 : 0));
  }
  if (cmd == "EXPIRE" || cmd == "PEXPIRE" || cmd == "PEXPIREAT") {
    const auto value = parse_i64(a[2]);
    if (!value) return integer_error();
    std::int64_t ms{};
    if (!checked_mul(*value, cmd == "EXPIRE" ? 1000 : 1, ms)) return integer_error();
    if (cmd != "PEXPIREAT" && ms > 0) {
      std::int64_t absolute{};
      if (!checked_add(clock_.unix_ms(), ms, absolute)) return integer_error();
    }
    return Reply::integer(cmd == "PEXPIREAT" ? store_.expire_at(a[1], ms)
                                             : store_.expire(a[1], ms));
  }
  if (cmd == "PERSIST") return Reply::integer(store_.persist(a[1]));
  if (cmd == "MSET") {
    if (a.size() % 2 != 1) return Reply::error("ERR wrong number of arguments for 'mset' command");
    std::vector<std::pair<std::string, std::string>> pairs;
    for (std::size_t i = 1; i < a.size(); i += 2) pairs.emplace_back(a[i], a[i + 1]);
    return set_reply(store_.set_many(pairs));
  }
  if (cmd == "MGET" || cmd == "KEYS") {
    Reply::Array out;
    if (cmd == "KEYS")
      for (auto& k : store_.keys(a[1])) out.push_back(Reply::bulk(std::move(k)));
    else
      for (std::size_t i = 1; i < a.size(); ++i) {
        auto v = store_.get(a[i]);
        out.push_back(v ? Reply::bulk(std::move(*v)) : Reply::null());
      }
    return Reply::array(std::move(out));
  }
  if (cmd == "GETSET" || cmd == "APPEND") {
    const auto old = store_.get(a[1], false);
    const auto ttl = store_.pttl(a[1]);
    std::string value = cmd == "GETSET" ? a[2] : old.value_or("") + a[2];
    const auto length = value.size();
    const auto result =
        store_.set(a[1], std::move(value),
                   cmd == "APPEND" && ttl >= 0 ? std::optional<std::int64_t>{ttl} : std::nullopt);
    if (result == SetResult::OutOfMemory) return oom();
    if (cmd == "APPEND") return Reply::integer(static_cast<std::int64_t>(length));
    return old ? Reply::bulk(*old) : Reply::null();
  }
  if (cmd == "INCR" || cmd == "DECR" || cmd == "INCRBY" || cmd == "DECRBY") {
    const auto old = store_.get(a[1], false);
    const auto base = old ? parse_i64(*old) : std::optional<std::int64_t>{0};
    const auto delta = n == 3 ? parse_i64(a[2]) : std::optional<std::int64_t>{1};
    if (!base || !delta) return integer_error();
    std::int64_t result{};
    const bool overflow = (cmd == "DECR" || cmd == "DECRBY")
                              ? __builtin_sub_overflow(*base, *delta, &result)
                              : !checked_add(*base, *delta, result);
    if (overflow) return Reply::error("ERR increment or decrement would overflow");
    const auto ttl = store_.pttl(a[1]);
    if (store_.set(a[1], std::to_string(result),
                   ttl >= 0 ? std::optional<std::int64_t>{ttl} : std::nullopt) ==
        SetResult::OutOfMemory)
      return oom();
    return Reply::integer(result);
  }
  if (cmd == "DBSIZE") return Reply::integer(static_cast<std::int64_t>(store_.size()));
  if (cmd == "FLUSHALL") {
    store_.clear();
    return Reply::ok();
  }
  if (cmd == "SAVE") {
    save_snapshot(path("dump.mrdb"), store_);
    return Reply::ok();
  }
  if (cmd == "BGREWRITEAOF") {
    if (!aof_) return Reply::error("ERR appendonly is disabled");
    if (persistence_failed_) return Reply::error("MISCONF persistence failed; restart required");
    try {
      aof_->rewrite(store_.records());
    } catch (...) {
      persistence_failed_ = true;
      throw;
    }
    return Reply::ok();
  }
  if (cmd == "SHUTDOWN") {
    if (!replay) {
      try {
        if (aof_) aof_->flush();
      } catch (...) {
        persistence_failed_ = true;
        throw;
      }
      shutdown_ = true;
    }
    return Reply::null();
  }
  if (cmd == "INFO") {
    if (n > 2) return Reply::error("ERR wrong number of arguments for 'info' command");
    const auto stats = store_.stats();
    std::ostringstream out;
    out << "# Server\r\nmini_redis_version:1.0.0\r\nredis_version:7.0.0\r\nuptime_in_seconds:"
        << (clock_.now_ms() - started_) / 1000 << "\r\nio_model:" << config_.io_model
        << "\r\n# Clients\r\nconnected_clients:" << clients.load()
        << "\r\n# Memory\r\nused_memory:" << store_.used_memory()
        << "\r\nmaxmemory:" << config_.maxmemory
        << "\r\n# Stats\r\ntotal_commands_processed:" << commands_
        << "\r\nkeyspace_hits:" << stats.hits << "\r\nkeyspace_misses:" << stats.misses
        << "\r\nevicted_keys:" << stats.evicted << "\r\nexpired_keys:" << stats.expired
        << "\r\n# Persistence\r\naof_enabled:" << static_cast<int>(config_.appendonly)
        << "\r\naof_write_error:" << static_cast<int>(persistence_failed_)
        << "\r\n# Keyspace\r\ndb0:keys=" << store_.size() << "\r\n";
    return Reply::bulk(out.str());
  }
  if (cmd == "CONFIG") {
    const auto sub = upper(a[1]);
    if (sub == "GET" && n == 3) {
      Reply::Array out;
      for (const auto* key :
           {"bind", "port", "dir", "appendonly", "appendfsync", "maxmemory", "maxmemory-policy",
            "io-model", "workers", "maxclients", "timeout", "client-output-buffer-limit",
            "proto-max-bulk-len", "aof-rewrite-min-size"})
        if (glob_match(a[2], key)) {
          out.push_back(Reply::bulk(key));
          out.push_back(Reply::bulk(config_value(config_, key)));
        }
      return Reply::array(std::move(out));
    }
    if (sub == "SET" && n == 4) {
      const auto key = lower(a[2]);
      if (key != "maxmemory" && key != "maxmemory-policy")
        return Reply::error("ERR configuration parameter is immutable at runtime");
      auto next = config_;
      set_config(next, key, a[3]);
      config_ = std::move(next);
      store_.set_memory_limit(config_.maxmemory, config_.memory_policy);
      return Reply::ok();
    }
    return Reply::error("ERR syntax error");
  }
  return Reply::error("ERR command not implemented");
}
}  // namespace mr
