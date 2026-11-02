// Fast-path microbenchmarks (Google Benchmark), compiled once per backend.
#include <benchmark/benchmark.h>

#include <thread>
#include <vector>

#include "backend.h"

namespace {

void BM_MallocFree(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  for (auto _ : state) {
    void* p = backend::Malloc(n);
    benchmark::DoNotOptimize(p);
    backend::Free(p);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MallocFree)->Arg(16)->Arg(64)->Arg(512)->Arg(4096)->Arg(32768);

void BM_Batch1000(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  std::vector<void*> v(1000);
  for (auto _ : state) {
    for (auto& p : v) p = backend::Malloc(n);
    benchmark::ClobberMemory();
    for (void* p : v) backend::Free(p);
  }
  state.SetItemsProcessed(state.iterations() * 1000);
}
BENCHMARK(BM_Batch1000)->Arg(32)->Arg(256)->Arg(2048);

// One thread allocates a batch, another frees it.
void BM_CrossThreadFree(benchmark::State& state) {
  std::vector<void*> v(1000);
  for (auto _ : state) {
    for (auto& p : v) p = backend::Malloc(64);
    std::thread([&] {
      for (void* p : v) backend::Free(p);
    }).join();
  }
  state.SetItemsProcessed(state.iterations() * 1000);
}
BENCHMARK(BM_CrossThreadFree)->UseRealTime();

}  // namespace

BENCHMARK_MAIN();
