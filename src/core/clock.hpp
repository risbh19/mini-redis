#pragma once
#include <chrono>
#include <cstdint>

namespace mr {
class IClock {
 public:
  virtual ~IClock() = default;
  virtual std::int64_t now_ms() const = 0;
  virtual std::int64_t unix_ms() const = 0;
};
class SystemClock final : public IClock {
 public:
  std::int64_t now_ms() const override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  std::int64_t unix_ms() const override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
};
class FakeClock final : public IClock {
 public:
  explicit FakeClock(std::int64_t unix_time = 1700000000000LL) : unix_(unix_time) {}
  std::int64_t now_ms() const override { return steady_; }
  std::int64_t unix_ms() const override { return unix_; }
  void advance(std::int64_t ms) {
    steady_ += ms;
    unix_ += ms;
  }
  void jump_wall(std::int64_t ms) { unix_ += ms; }

 private:
  std::int64_t steady_{};
  std::int64_t unix_;
};
}  // namespace mr
