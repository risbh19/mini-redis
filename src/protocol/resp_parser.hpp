#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace mr {
struct ParserLimits {
  std::size_t max_bulk{512ULL * 1024 * 1024};
  std::size_t max_array{1024ULL * 1024};
  std::size_t max_inline{64ULL * 1024};
  std::size_t max_request{513ULL * 1024 * 1024};
};
enum class ParseStatus : std::uint8_t { Complete, Incomplete, ProtocolError };
struct ParseResult {
  ParseStatus status{ParseStatus::Incomplete};
  std::vector<std::string> args;
  std::size_t consumed{};
  std::string error;
};
class RespParser {
 public:
  explicit RespParser(ParserLimits limits = {}) : limits_(limits) {}
  ParseResult parse(std::string_view input) const;

 private:
  ParserLimits limits_;
};
}  // namespace mr
