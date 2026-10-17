#include "util/string_utils.hpp"

#include <cctype>
#include <charconv>
#include <limits>
namespace mr {
std::string upper(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}
std::string lower(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}
std::optional<std::int64_t> parse_i64(std::string_view s) {
  if (s.empty()) return std::nullopt;
  std::int64_t result{};
  const auto parsed = std::from_chars(s.data(), s.data() + s.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size()) return std::nullopt;
  return result;
}
bool checked_add(std::int64_t a, std::int64_t b, std::int64_t& out) {
  if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
      (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b))
    return false;
  out = a + b;
  return true;
}
bool checked_mul(std::int64_t a, std::int64_t b, std::int64_t& out) {
  return !__builtin_mul_overflow(a, b, &out);
}
// Greedy star backtracking, with bracket classes and escapes. No recursive stack growth.
bool glob_match(std::string_view p, std::string_view s) {
  std::size_t pi = 0, si = 0, star = std::string_view::npos, retry = 0;
  auto token = [&](std::size_t& i, char c) {
    if (p[i] == '?') {
      ++i;
      return true;
    }
    if (p[i] == '\\' && i + 1 < p.size()) {
      ++i;
      return p[i++] == c;
    }
    if (p[i] == '[') {
      const auto end = p.find(']', i + 1);
      if (end != std::string_view::npos) {
        ++i;
        bool negate = false, match = false;
        if (i < end && (p[i] == '^' || p[i] == '!')) {
          negate = true;
          ++i;
        }
        while (i < end) {
          if (i + 2 < end && p[i + 1] == '-') {
            match = match || (c >= p[i] && c <= p[i + 2]);
            i += 3;
          } else
            match = (p[i++] == c) || match;
        }
        i = end + 1;
        return match != negate;
      }
    }
    return p[i++] == c;
  };
  while (si < s.size()) {
    if (pi < p.size() && p[pi] == '*') {
      star = pi++;
      retry = si;
      continue;
    }
    auto next = pi;
    if (pi < p.size() && token(next, s[si])) {
      pi = next;
      ++si;
      continue;
    }
    if (star == std::string_view::npos) return false;
    pi = star + 1;
    si = ++retry;
  }
  while (pi < p.size() && p[pi] == '*') ++pi;
  return pi == p.size();
}
bool split_inline(std::string_view line, std::vector<std::string>& args, std::string& error) {
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    if (i == line.size()) break;
    std::string word;
    char quote = 0;
    while (i < line.size()) {
      const char c = line[i++];
      if (!quote && std::isspace(static_cast<unsigned char>(c))) break;
      if (c == '\\' && i < line.size()) {
        const char escaped = line[i++];
        switch (escaped) {
          case 'n':
            word += '\n';
            break;
          case 'r':
            word += '\r';
            break;
          case 't':
            word += '\t';
            break;
          default:
            word += escaped;
        }
      } else if ((c == '"' || c == '\'') && !quote)
        quote = c;
      else if (c == quote)
        quote = 0;
      else
        word += c;
    }
    if (quote) {
      error = "unbalanced quotes";
      return false;
    }
    args.push_back(std::move(word));
  }
  return true;
}
std::string safe_line(std::string_view s) {
  std::string out(s);
  for (auto& c : out)
    if (c == '\r' || c == '\n' || c == '\0') c = ' ';
  return out;
}
}  // namespace mr
