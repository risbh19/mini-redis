#pragma once
#include <string>
#include <vector>

#include "protocol/reply.hpp"
namespace mr {
std::string resp_encode(const Reply& reply);
void resp_append(std::string& out, const Reply& reply);
std::string encode_command(const std::vector<std::string>& args);
}  // namespace mr
