#include "protocol/resp_writer.hpp"

#include <type_traits>

#include "util/string_utils.hpp"
namespace mr {
void resp_append(std::string& out, const Reply& reply) {
  std::visit(
      [&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, SimpleString>)
          out += "+" + safe_line(v.value) + "\r\n";
        else if constexpr (std::is_same_v<T, Error>)
          out += "-" + safe_line(v.value) + "\r\n";
        else if constexpr (std::is_same_v<T, std::int64_t>)
          out += ":" + std::to_string(v) + "\r\n";
        else if constexpr (std::is_same_v<T, Bulk>)
          out += "$" + std::to_string(v.value.size()) + "\r\n" + v.value + "\r\n";
        else if constexpr (std::is_same_v<T, Null>)
          out += "$-1\r\n";
        else {
          out += "*" + std::to_string(v.size()) + "\r\n";
          for (const auto& e : v) resp_append(out, e);
        }
      },
      reply.value);
}
std::string resp_encode(const Reply& reply) {
  std::string out;
  resp_append(out, reply);
  return out;
}
std::string encode_command(const std::vector<std::string>& args) {
  std::string out = "*" + std::to_string(args.size()) + "\r\n";
  for (const auto& arg : args) out += "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
  return out;
}
}  // namespace mr
