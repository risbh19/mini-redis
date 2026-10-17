#include <cstddef>
#include <cstdint>
#include <string_view>

#include "persistence/aof.hpp"
// Same in-memory decoder used at startup: framing, transactions and torn-tail recovery.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);
  try {
    const auto result = mr::decode_aof(bytes, [](const auto& args) {
      if (args.empty()) __builtin_trap();
    });
    if (result.recovered_bytes > size) __builtin_trap();
  } catch (const std::exception&) {
  }
  return 0;
}
