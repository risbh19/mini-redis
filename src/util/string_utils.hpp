#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace mr {
std::string upper(std::string_view s);
std::string lower(std::string_view s);
std::optional<std::int64_t> parse_i64(std::string_view s);
bool checked_add(std::int64_t a, std::int64_t b, std::int64_t& out);
bool checked_mul(std::int64_t a, std::int64_t b, std::int64_t& out);
bool glob_match(std::string_view pattern, std::string_view text);
bool split_inline(std::string_view line, std::vector<std::string>& args, std::string& error);
std::string safe_line(std::string_view s);
}  // namespace mr
