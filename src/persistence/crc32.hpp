#pragma once
#include <cstdint>
#include <string_view>
namespace mr {
std::uint32_t crc32(std::string_view bytes);
}
