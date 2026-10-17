#include "protocol/resp_parser.hpp"

#include <algorithm>

#include "util/string_utils.hpp"
namespace mr {
ParseResult RespParser::parse(std::string_view input) const {
  ParseResult r;
  auto fail = [&](std::string error) {
    r.status = ParseStatus::ProtocolError;
    r.error = std::move(error);
    return r;
  };
  if (input.empty()) return r;
  if (input.front() != '*') {
    const auto end = input.find('\n');
    if ((end == std::string_view::npos ? input.size() : end) > limits_.max_inline)
      return fail("inline request too long");
    if (end == std::string_view::npos) return r;
    auto line = input.substr(0, end);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!split_inline(line, r.args, r.error)) return fail(r.error);
    if (r.args.size() > limits_.max_array) return fail("too many arguments");
    r.consumed = end + 1;
    r.status = ParseStatus::Complete;
    return r;
  }
  std::size_t pos = 1;
  auto number = [&](std::int64_t& n) -> int {
    const auto end = input.find("\r\n", pos);
    if (end == std::string_view::npos) return input.size() - pos > 24 ? -1 : 0;
    if (end - pos > 24) return -1;
    auto value = parse_i64(input.substr(pos, end - pos));
    if (!value) return -1;
    n = *value;
    pos = end + 2;
    return 1;
  };
  std::int64_t count{};
  const int header = number(count);
  if (header == 0) return r;
  if (header < 0 || count <= 0 || static_cast<std::uint64_t>(count) > limits_.max_array)
    return fail("invalid array length");
  r.args.reserve(std::min(static_cast<std::size_t>(count), std::size_t{64}));
  for (std::int64_t i = 0; i < count; ++i) {
    if (pos >= input.size()) return r;
    if (input[pos++] != '$') return fail("expected bulk string");
    std::int64_t length{};
    const int parsed = number(length);
    if (parsed == 0) return r;
    if (parsed < 0 || length < 0 || static_cast<std::uint64_t>(length) > limits_.max_bulk)
      return fail("invalid bulk length");
    const auto len = static_cast<std::size_t>(length);
    if (pos > limits_.max_request || len > limits_.max_request - pos ||
        limits_.max_request - pos - len < 2)
      return fail("request too large");
    if (input.size() - pos < len + 2) return r;
    if (input[pos + len] != '\r' || input[pos + len + 1] != '\n')
      return fail("invalid bulk terminator");
    r.args.emplace_back(input.substr(pos, len));
    pos += len + 2;
  }
  r.consumed = pos;
  r.status = ParseStatus::Complete;
  return r;
}
}  // namespace mr
