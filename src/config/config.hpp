#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/store.hpp"
#include "persistence/aof.hpp"
#include "protocol/resp_parser.hpp"
namespace mr {
struct Config {
  std::string bind{"127.0.0.1"};
  std::uint16_t port{6380};
  std::string dir{".data"};
  bool appendonly{true};
  FsyncPolicy appendfsync{FsyncPolicy::EverySecond};
  std::size_t maxmemory{};
  EvictionPolicy memory_policy{EvictionPolicy::NoEviction};
  std::string io_model{"epoll"};
  std::size_t workers{4}, maxclients{10000};
  std::int64_t idle_timeout_ms{0};
  std::size_t output_limit{8ULL * 1024 * 1024};
  std::size_t aof_rewrite_min{64ULL * 1024 * 1024};
  ParserLimits parser_limits;
  bool cli{}, help{};
};
Config parse_config(int argc, char** argv);
void set_config(Config& config, const std::string& key, const std::string& value);
std::string config_value(const Config& config, const std::string& key);
std::string config_help();
}  // namespace mr
