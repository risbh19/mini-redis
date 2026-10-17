#include "persistence/crc32.hpp"
namespace mr {
std::uint32_t crc32(std::string_view bytes) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const char c : bytes) {
    crc ^= static_cast<unsigned char>(c);
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return crc ^ 0xFFFFFFFFU;
}
}  // namespace mr
