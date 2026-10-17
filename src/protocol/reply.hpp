#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>
namespace mr {
struct SimpleString {
  std::string value;
};
struct Error {
  std::string value;
};
struct Bulk {
  std::string value;
};
struct Null {};
struct Reply {
  using Array = std::vector<Reply>;
  using Value = std::variant<SimpleString, Error, std::int64_t, Bulk, Null, Array>;
  Value value{Null{}};
  static Reply simple(std::string s) { return {SimpleString{std::move(s)}}; }
  static Reply ok() { return simple("OK"); }
  static Reply error(std::string s) { return {Error{std::move(s)}}; }
  static Reply integer(std::int64_t n) { return {n}; }
  static Reply bulk(std::string s) { return {Bulk{std::move(s)}}; }
  static Reply null() { return {Null{}}; }
  static Reply array(Array a) { return {std::move(a)}; }
  bool is_error() const { return std::holds_alternative<Error>(value); }
};
}  // namespace mr
