#include "util/logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
namespace mr {
void log(std::string_view level, std::string_view message) {
  static std::mutex mutex;
  const std::lock_guard<std::mutex> guard(mutex);
  const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm utc{};
  gmtime_r(&time, &utc);
  std::cerr << "{\"time\":\"" << std::put_time(&utc, "%FT%TZ")
            << "\",\"level\":" << std::quoted(std::string(level))
            << ",\"message\":" << std::quoted(std::string(message)) << "}\n";
}
}  // namespace mr
