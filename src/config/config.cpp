#include "config/config.hpp"

#include <fstream>
#include <limits>
#include <stdexcept>

#include "util/string_utils.hpp"
namespace mr {
namespace {
std::size_t number(const std::string& value) {
  const auto parsed = parse_i64(value);
  if (!parsed || *parsed < 0) throw std::runtime_error("expected nonnegative integer: " + value);
  return static_cast<std::size_t>(*parsed);
}
std::string policy(FsyncPolicy p) {
  return p == FsyncPolicy::Always ? "always" : p == FsyncPolicy::No ? "no" : "everysec";
}
}  // namespace
// Name/value order follows CLI configuration; invalid names fail validation.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void set_config(Config& c, const std::string& name, const std::string& v) {
  const auto k = lower(name);
  if (k == "bind")
    c.bind = v;
  else if (k == "port") {
    const auto n = number(v);
    if (n > 65535) throw std::runtime_error("port out of range");
    c.port = static_cast<std::uint16_t>(n);
  } else if (k == "dir") {
    if (v.empty()) throw std::runtime_error("dir cannot be empty");
    c.dir = v;
  } else if (k == "appendonly") {
    if (v != "yes" && v != "no") throw std::runtime_error("appendonly expects yes|no");
    c.appendonly = v == "yes";
  } else if (k == "appendfsync") {
    if (v == "always")
      c.appendfsync = FsyncPolicy::Always;
    else if (v == "everysec")
      c.appendfsync = FsyncPolicy::EverySecond;
    else if (v == "no")
      c.appendfsync = FsyncPolicy::No;
    else
      throw std::runtime_error("appendfsync expects always|everysec|no");
  } else if (k == "maxmemory")
    c.maxmemory = number(v);
  else if (k == "maxmemory-policy") {
    if (v == "allkeys-lru")
      c.memory_policy = EvictionPolicy::AllKeysLru;
    else if (v == "noeviction")
      c.memory_policy = EvictionPolicy::NoEviction;
    else
      throw std::runtime_error("maxmemory-policy expects noeviction|allkeys-lru");
  } else if (k == "io-model") {
    if (v != "epoll" && v != "thread-pool")
      throw std::runtime_error("io-model expects epoll|thread-pool");
    c.io_model = v;
  } else if (k == "workers") {
    c.workers = number(v);
    if (!c.workers || c.workers > 1024) throw std::runtime_error("workers must be 1..1024");
  } else if (k == "maxclients") {
    c.maxclients = number(v);
    if (!c.maxclients) throw std::runtime_error("maxclients must be positive");
  } else if (k == "timeout") {
    const auto n = number(v);
    if (n > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() / 1000))
      throw std::runtime_error("timeout out of range");
    c.idle_timeout_ms = static_cast<std::int64_t>(n) * 1000;
  } else if (k == "client-output-buffer-limit") {
    c.output_limit = number(v);
    if (c.output_limit < 1024) throw std::runtime_error("output limit must be >= 1024");
  } else if (k == "proto-max-bulk-len") {
    c.parser_limits.max_bulk = number(v);
    if (c.parser_limits.max_bulk > 512ULL * 1024 * 1024)
      throw std::runtime_error("bulk limit must be <= 512 MiB");
  } else if (k == "aof-rewrite-min-size")
    c.aof_rewrite_min = number(v);
  else
    throw std::runtime_error("unknown configuration option: " + name);
}
std::string config_value(const Config& c, const std::string& k) {
  if (k == "bind") return c.bind;
  if (k == "port") return std::to_string(c.port);
  if (k == "dir") return c.dir;
  if (k == "appendonly") return c.appendonly ? "yes" : "no";
  if (k == "appendfsync") return policy(c.appendfsync);
  if (k == "maxmemory") return std::to_string(c.maxmemory);
  if (k == "maxmemory-policy")
    return c.memory_policy == EvictionPolicy::AllKeysLru ? "allkeys-lru" : "noeviction";
  if (k == "io-model") return c.io_model;
  if (k == "workers") return std::to_string(c.workers);
  if (k == "maxclients") return std::to_string(c.maxclients);
  if (k == "timeout") return std::to_string(c.idle_timeout_ms / 1000);
  if (k == "client-output-buffer-limit") return std::to_string(c.output_limit);
  if (k == "proto-max-bulk-len") return std::to_string(c.parser_limits.max_bulk);
  if (k == "aof-rewrite-min-size") return std::to_string(c.aof_rewrite_min);
  return {};
}
Config parse_config(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      c.help = true;
      continue;
    }
    if (arg == "--cli") {
      c.cli = true;
      continue;
    }
    if (arg == "--config") {
      if (++i == argc) throw std::runtime_error("--config needs a file");
      std::ifstream in(argv[i]);
      if (!in) throw std::runtime_error("cannot read config");
      std::string line;
      std::size_t lineno = 0;
      while (std::getline(in, line)) {
        ++lineno;
        std::vector<std::string> args;
        std::string error;
        if (const auto comment = line.find('#'); comment != std::string::npos) line.resize(comment);
        if (!split_inline(line, args, error) || (!args.empty() && args.size() != 2))
          throw std::runtime_error("invalid config line " + std::to_string(lineno));
        if (!args.empty()) set_config(c, args[0], args[1]);
      }
    } else {
      if (arg.rfind("--", 0) != 0 || ++i == argc)
        throw std::runtime_error("expected --option value; try --help");
      set_config(c, arg.substr(2), argv[i]);
    }
  }
  return c;
}
std::string config_help() {
  return "mini-redis 1.0.0 (Linux, RESP2)\nUsage: mini-redis [--config mini-redis.conf] [--cli] "
         "[options]\n"
         "  --bind IPv4 --port 6380 --dir .data\n  --appendonly yes|no --appendfsync "
         "always|everysec|no\n"
         "  --io-model epoll|thread-pool --workers 4 --maxclients 10000\n"
         "  --maxmemory BYTES --maxmemory-policy noeviction|allkeys-lru\n"
         "  --timeout SECONDS --client-output-buffer-limit BYTES\n"
         "  --proto-max-bulk-len BYTES --aof-rewrite-min-size BYTES\n";
}
}  // namespace mr
