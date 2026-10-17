#include <benchmark/benchmark.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "core/hash_table.hpp"
#include "core/store.hpp"
template <class Table>
void insert(benchmark::State& state) {
  std::vector<std::string> keys;
  for (std::int64_t i = 0; i < state.range(0); ++i) keys.push_back(std::to_string(i));
  for (auto _ : state) {
    (void)_;
    Table table;
    for (const auto& key : keys) table.insert_or_assign(key, "value");
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
template <class Table>
void lookup(benchmark::State& state) {
  Table table;
  std::vector<std::string> keys;
  for (std::int64_t i = 0; i < state.range(0); ++i) {
    keys.push_back(std::to_string(i));
    table.insert_or_assign(keys.back(), "value");
  }
  std::size_t cursor = 0;
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(table.find(keys[cursor++ % keys.size()]));
  }
}
template <class Table>
void miss(benchmark::State& state) {
  Table table;
  for (std::int64_t i = 0; i < state.range(0); ++i)
    table.insert_or_assign(std::to_string(i), "value");
  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(table.find("missing"));
  }
}
template <class Table>
void erase(benchmark::State& state) {
  std::vector<std::string> keys;
  for (std::int64_t i = 0; i < state.range(0); ++i) keys.push_back(std::to_string(i));
  for (auto _ : state) {
    (void)_;
    state.PauseTiming();
    Table table;
    for (const auto& key : keys) table.insert_or_assign(key, "value");
    state.ResumeTiming();
    for (const auto& key : keys) benchmark::DoNotOptimize(table.erase(key));
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
using Custom = mr::HashTable<std::string>;
using Standard = std::unordered_map<std::string, std::string>;
BENCHMARK_TEMPLATE(insert, Custom)->Arg(1000)->Arg(100000)->Arg(1000000);
BENCHMARK_TEMPLATE(insert, Standard)->Arg(1000)->Arg(100000)->Arg(1000000);
BENCHMARK_TEMPLATE(lookup, Custom)->Arg(1000)->Arg(100000)->Arg(1000000);
BENCHMARK_TEMPLATE(lookup, Standard)->Arg(1000)->Arg(100000)->Arg(1000000);
BENCHMARK_TEMPLATE(miss, Custom)->Arg(1000)->Arg(100000)->Arg(1000000);
BENCHMARK_TEMPLATE(miss, Standard)->Arg(1000)->Arg(100000)->Arg(1000000);
BENCHMARK_TEMPLATE(erase, Custom)->Arg(1000)->Arg(100000)->Arg(1000000);
BENCHMARK_TEMPLATE(erase, Standard)->Arg(1000)->Arg(100000)->Arg(1000000);
