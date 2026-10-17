#pragma once
#include <string>
#include <vector>

#include "core/store.hpp"
namespace mr {
std::string encode_snapshot(const std::vector<Record>& records);
std::vector<Record> decode_snapshot(const std::string& bytes);
void save_snapshot(const std::string& path, const Store& store);
void load_snapshot(const std::string& path, Store& store);
}  // namespace mr
