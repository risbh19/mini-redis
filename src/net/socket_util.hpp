#pragma once
#include <cstdint>
#include <string>
#include <utility>
namespace mr {
class UniqueFd {
 public:
  explicit UniqueFd(int fd = -1) : fd_(fd) {}
  ~UniqueFd();
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept;
  int get() const { return fd_; }
  explicit operator bool() const { return fd_ >= 0; }
  int release() { return std::exchange(fd_, -1); }
  void reset(int fd = -1);

 private:
  int fd_;
};
void nonblocking(int fd);
void tcp_nodelay(int fd);
UniqueFd listen_tcp(const std::string& address, std::uint16_t port, bool nonblock);
UniqueFd connect_tcp(const std::string& address, std::uint16_t port);
void write_all(int fd, const char* data, std::size_t size);
void sync_fd(int fd);
void atomic_file(const std::string& path, const std::string& bytes);
std::string read_file(const std::string& path, std::size_t max_bytes = 1024ULL * 1024 * 1024);
}  // namespace mr
