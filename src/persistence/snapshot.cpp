#include "persistence/snapshot.hpp"

#include <limits>
#include <stdexcept>
#include <unordered_set>

#include "net/socket_util.hpp"
#include "persistence/crc32.hpp"
namespace mr {
namespace {
void le(std::string& out, std::uint64_t n, unsigned width) {
  for (unsigned i = 0; i < width; ++i) out += static_cast<char>((n >> (i * 8U)) & 0xFFU);
}
std::uint64_t read_le(const std::string& bytes, std::size_t& pos, unsigned width, std::size_t end) {
  if (pos > end || end - pos < width) throw std::runtime_error("truncated snapshot");
  std::uint64_t n = 0;
  for (unsigned i = 0; i < width; ++i)
    n |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[pos++])) << (i * 8U);
  return n;
}
}  // namespace
std::string encode_snapshot(const std::vector<Record>& records) {
  std::string out = "MRDB\x01";
  for (const auto& r : records) {
    if (r.key.size() > std::numeric_limits<std::uint32_t>::max() ||
        r.value.size() > std::numeric_limits<std::uint32_t>::max())
      throw std::runtime_error("snapshot string too long");
    out += '\x01';
    le(out, static_cast<std::uint64_t>(r.expire_at_ms.value_or(0)), 8);
    le(out, r.key.size(), 4);
    out += r.key;
    le(out, r.value.size(), 4);
    out += r.value;
  }
  out += static_cast<char>(0xFF);
  le(out, crc32(out), 4);
  return out;
}
std::vector<Record> decode_snapshot(const std::string& bytes) {
  if (bytes.size() < 10 || bytes.compare(0, 5, "MRDB\x01", 5) != 0)
    throw std::runtime_error("invalid snapshot magic/version");
  const auto end = bytes.size() - 4;
  auto checksum_pos = end;
  if (read_le(bytes, checksum_pos, 4, bytes.size()) !=
      crc32(std::string_view(bytes).substr(0, end)))
    throw std::runtime_error("snapshot CRC32 mismatch");
  std::size_t pos = 5;
  std::vector<Record> records;
  std::unordered_set<std::string> seen;
  while (pos < end) {
    const auto opcode = static_cast<unsigned char>(bytes[pos++]);
    if (opcode == 0xFF) {
      if (pos != end) throw std::runtime_error("trailing snapshot data");
      return records;
    }
    if (opcode != 1) throw std::runtime_error("invalid snapshot opcode");
    const auto expiry = read_le(bytes, pos, 8, end);
    if (expiry > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      throw std::runtime_error("invalid snapshot expiry");
    auto string = [&] {
      const auto len = read_le(bytes, pos, 4, end);
      if (len > end - pos || len > 512ULL * 1024 * 1024)
        throw std::runtime_error("invalid snapshot string length");
      auto s = bytes.substr(pos, static_cast<std::size_t>(len));
      pos += static_cast<std::size_t>(len);
      return s;
    };
    Record r;
    r.key = string();
    r.value = string();
    if (expiry) r.expire_at_ms = static_cast<std::int64_t>(expiry);
    if (!seen.insert(r.key).second) throw std::runtime_error("duplicate snapshot key");
    records.push_back(std::move(r));
  }
  throw std::runtime_error("missing snapshot EOF");
}
void save_snapshot(const std::string& path, const Store& store) {
  atomic_file(path, encode_snapshot(store.records()));
}
void load_snapshot(const std::string& path, Store& store) {
  store.restore(decode_snapshot(read_file(path)));
}
}  // namespace mr
