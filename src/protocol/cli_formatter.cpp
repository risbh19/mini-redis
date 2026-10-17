#include "protocol/cli_formatter.hpp"

#include <iomanip>
#include <sstream>
#include <type_traits>
namespace mr {
std::string format_cli(const Reply& reply) {
  return std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, SimpleString>)
          return v.value;
        else if constexpr (std::is_same_v<T, Error>)
          return "(error) " + v.value;
        else if constexpr (std::is_same_v<T, std::int64_t>)
          return "(integer) " + std::to_string(v);
        else if constexpr (std::is_same_v<T, Bulk>) {
          std::ostringstream out;
          out << std::quoted(v.value);
          return out.str();
        } else if constexpr (std::is_same_v<T, Null>)
          return "(nil)";
        else {
          std::string out;
          for (std::size_t i = 0; i < v.size(); ++i)
            out += std::to_string(i + 1) + ") " + format_cli(v[i]) + "\n";
          return out.empty() ? "(empty array)" : out;
        }
      },
      reply.value);
}
}  // namespace mr
