#include <cstddef>
#include <cstdint>
#include <string_view>

#include "protocol/resp_parser.hpp"
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  mr::ParserLimits limits;
  limits.max_bulk = 1024 * 1024;
  limits.max_array = 1024;
  limits.max_request = 1024 * 1024;
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);
  mr::RespParser parser(limits);
  std::size_t pos = 0;
  while (pos < size) {
    const auto r = parser.parse(bytes.substr(pos));
    if (r.status != mr::ParseStatus::Complete) break;
    if (!r.consumed || r.consumed > size - pos) __builtin_trap();
    pos += r.consumed;
  }
  return 0;
}
