#pragma once
#include <cstdint>
#include <string>

#include "net/socket_util.hpp"
#include "protocol/resp_parser.hpp"
namespace mr {
struct Connection {
  UniqueFd fd;
  RespParser parser;
  std::string input, output;
  std::size_t sent{};
  std::int64_t last_activity{};
  bool closing{}, read_eof{};
  Connection(UniqueFd socket, ParserLimits limits, std::int64_t now)
      : fd(std::move(socket)), parser(limits), last_activity(now) {}
  std::size_t pending() const { return output.size() - sent; }
};
}  // namespace mr
