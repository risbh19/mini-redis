#include "net/socket_util.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
namespace mr {
namespace {
std::runtime_error sys_error(const std::string& text) {
  return std::runtime_error(text + ": " + std::strerror(errno));
}
}  // namespace
UniqueFd::~UniqueFd() { reset(); }
UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
  if (this != &other) reset(other.release());
  return *this;
}
void UniqueFd::reset(int fd) {
  if (fd_ >= 0) ::close(fd_);
  fd_ = fd;
}
void nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) throw sys_error("fcntl");
}
void tcp_nodelay(int fd) {
  int yes = 1;
  if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0)
    throw sys_error("TCP_NODELAY");
}
UniqueFd listen_tcp(const std::string& address, std::uint16_t port, bool nonblock) {
  UniqueFd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!fd) throw sys_error("socket");
  int yes = 1;
  if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
    throw sys_error("SO_REUSEADDR");
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1)
    throw std::runtime_error("bind must be an IPv4 address");
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    throw sys_error("bind");
  if (::listen(fd.get(), 1024) < 0) throw sys_error("listen");
  if (nonblock) nonblocking(fd.get());
  return fd;
}
UniqueFd connect_tcp(const std::string& address, std::uint16_t port) {
  UniqueFd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!fd) throw sys_error("socket");
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1)
    throw std::runtime_error("host must be IPv4");
  if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    throw sys_error("connect");
  tcp_nodelay(fd.get());
  return fd;
}
void write_all(int fd, const char* data, std::size_t size) {
  while (size) {
    const auto n = ::write(fd, data, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw sys_error("write");
    const auto written = static_cast<std::size_t>(n);
    data += written;
    size -= written;
  }
}
void sync_fd(int fd) {
  while (::fsync(fd) < 0) {
    if (errno != EINTR) throw sys_error("fsync");
  }
}
// Destination/content order follows file-writing APIs; both parameters are owned by callers.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void atomic_file(const std::string& path, const std::string& bytes) {
  const auto temp = path + ".tmp";
  UniqueFd fd(::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
  if (!fd) throw sys_error("open " + temp);
  write_all(fd.get(), bytes.data(), bytes.size());
  sync_fd(fd.get());
  fd.reset();
  if (::rename(temp.c_str(), path.c_str()) < 0) throw sys_error("rename " + path);
  auto parent = std::filesystem::path(path).parent_path();
  if (parent.empty()) parent = ".";
  UniqueFd dir(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!dir) throw sys_error("open directory");
  sync_fd(dir.get());
}
std::string read_file(const std::string& path, std::size_t max_bytes) {
  UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (!fd) throw sys_error("open " + path);
  struct stat st{};
  if (::fstat(fd.get(), &st) < 0) throw sys_error("fstat");
  if (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > max_bytes)
    throw std::runtime_error("file exceeds configured load limit");
  std::string bytes(static_cast<std::size_t>(st.st_size), '\0');
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto n = ::read(fd.get(), bytes.data() + offset, bytes.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("short read: " + path);
    offset += static_cast<std::size_t>(n);
  }
  return bytes;
}
}  // namespace mr
