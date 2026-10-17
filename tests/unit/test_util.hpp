#pragma once
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
struct TempDir {
  std::string path;
  TempDir() {
    std::string pattern = "/tmp/mini-redis-test-XXXXXX";
    auto* result = ::mkdtemp(pattern.data());
    if (!result) throw std::runtime_error("mkdtemp");
    path = result;
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::string file(const std::string& name) const { return path + "/" + name; }
};
