// Benchmark driver. One executable per backend (see backend.h). Each invocation runs one
// workload once and prints a single JSON line; bench/scripts/run.sh repeats invocations.
//
//   bench_<backend> replay <trace.hwt>
//   bench_<backend> larson   <threads> <iters/thread>
//   bench_<backend> prodcon  <pairs>   <items/producer>
//   bench_<backend> churn    <threads> <rounds>
//   bench_<backend> fragment <millions of objects>
#include <sys/resource.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "backend.h"
#include "trace_format.h"

namespace {

using Clock = std::chrono::steady_clock;

double PeakRssMb() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
  return static_cast<double>(ru.ru_maxrss) / 1048576.0;  // bytes
#else
  return static_cast<double>(ru.ru_maxrss) / 1024.0;  // kilobytes
#endif
}

void Report(const char* workload, const std::string& detail, double seconds, std::uint64_t ops, double rss_base) {
  std::printf(
      "{\"backend\":\"%s\",\"version\":\"%s\",\"workload\":\"%s\",\"detail\":\"%s\",\"seconds\":%.6f,"
      "\"ops\":%llu,\"mops_per_s\":%.3f,\"peak_rss_mb\":%.1f,\"rss_base_mb\":%.1f}\n",
      backend::Name(), backend::Version(), workload, detail.c_str(), seconds, static_cast<unsigned long long>(ops),
      static_cast<double>(ops) / seconds / 1e6, PeakRssMb(), rss_base);
}

// Write the first word and one byte per 4 KiB, like a program initialising its object.
inline void Touch(void* p, std::size_t n) {
  if (p == nullptr || n == 0) return;
  auto* c = static_cast<volatile unsigned char*>(p);
  c[0] = 1;
  for (std::size_t i = 4096; i < n; i += 4096) c[i] = 1;
}

int Replay(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    std::perror(path);
    return 1;
  }
  trace::Header h{};
  if (std::fread(&h, sizeof(h), 1, f) != 1 || std::memcmp(h.magic, trace::kMagic, 4) != 0) {
    std::fprintf(stderr, "%s: not a .hwt trace\n", path);
    return 1;
  }
  std::vector<trace::Op> ops(h.ops);
  if (std::fread(ops.data(), sizeof(trace::Op), ops.size(), f) != ops.size()) {
    std::fprintf(stderr, "%s: truncated\n", path);
    return 1;
  }
  std::fclose(f);
  std::vector<void*> slot(h.ids, nullptr);
  const double base = PeakRssMb();
  const auto t0 = Clock::now();
  for (const trace::Op& op : ops) {
    switch (op.kind) {
      case trace::kAlloc:
        slot[op.id] = backend::Malloc(op.size);
        Touch(slot[op.id], op.size);
        break;
      case trace::kCalloc:
        slot[op.id] = backend::Calloc(op.size);
        Touch(slot[op.id], op.size);
        break;
      case trace::kAligned:
        slot[op.id] = backend::Aligned(std::size_t{1} << op.align_log2, op.size);
        Touch(slot[op.id], op.size);
        break;
      case trace::kRealloc:
        slot[op.id] = backend::Realloc(slot[op.id], op.size);
        Touch(slot[op.id], op.size);
        break;
      case trace::kFree:
        backend::Free(slot[op.id]);
        slot[op.id] = nullptr;
        break;
      default:
        std::abort();
    }
  }
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  std::string name = path;
  if (auto pos = name.find_last_of('/'); pos != std::string::npos) name = name.substr(pos + 1);
  Report("replay", name, secs, ops.size(), base);
  return 0;
}

// Larson-style server simulation: each thread owns a window of slots and repeatedly
// replaces a random slot. Every round, windows rotate between threads, so a large share
// of frees happens on a thread other than the allocating one.
int Larson(int threads, int iters) {
  constexpr int kSlots = 4096;
  constexpr int kRounds = 8;
  std::vector<std::vector<void*>> windows(static_cast<std::size_t>(threads), std::vector<void*>(kSlots, nullptr));
  const double base = PeakRssMb();
  const auto t0 = Clock::now();
  for (int round = 0; round < kRounds; ++round) {
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
      ts.emplace_back([&, t] {
        auto& w = windows[static_cast<std::size_t>((t + round) % threads)];
        std::mt19937 rng(static_cast<unsigned>(t * 131 + round));
        for (int i = 0; i < iters / kRounds; ++i) {
          const std::size_t idx = rng() % kSlots;
          backend::Free(w[idx]);
          const std::size_t n = 16 + rng() % 1008;
          w[idx] = backend::Malloc(n);
          static_cast<volatile char*>(w[idx])[0] = 1;
        }
      });
    }
    for (auto& t : ts) t.join();
  }
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  for (auto& w : windows) {
    for (void* p : w) backend::Free(p);
  }
  Report("larson", std::to_string(threads) + "t", secs, static_cast<std::uint64_t>(threads) * iters, base);
  return 0;
}

// Producer/consumer pairs connected by single-producer/single-consumer rings: every free is
// cross-thread, the pattern of message-passing services and pipelines.
int ProdCon(int pairs, int items) {
  constexpr std::size_t kCap = 4096;
  struct alignas(128) Ring {
    std::vector<void*> buf = std::vector<void*>(4096);
    alignas(128) std::atomic<std::size_t> head{0};
    alignas(128) std::atomic<std::size_t> tail{0};
  };
  std::vector<Ring> rings(static_cast<std::size_t>(pairs));
  const double base = PeakRssMb();
  const auto t0 = Clock::now();
  std::vector<std::thread> ts;
  for (int p = 0; p < pairs; ++p) {
    Ring& r = rings[static_cast<std::size_t>(p)];
    ts.emplace_back([&r, items, p] {
      std::mt19937 rng(static_cast<unsigned>(p));
      for (int i = 0; i < items; ++i) {
        const std::size_t n = 32 + rng() % 480;
        void* m = backend::Malloc(n);
        static_cast<volatile char*>(m)[0] = 1;
        const std::size_t h = r.head.load(std::memory_order_relaxed);
        while (h - r.tail.load(std::memory_order_acquire) == kCap) std::this_thread::yield();
        r.buf[h % kCap] = m;
        r.head.store(h + 1, std::memory_order_release);
      }
    });
    ts.emplace_back([&r, items] {
      for (int i = 0; i < items; ++i) {
        const std::size_t t = r.tail.load(std::memory_order_relaxed);
        while (r.head.load(std::memory_order_acquire) == t) std::this_thread::yield();
        backend::Free(r.buf[t % kCap]);
        r.tail.store(t + 1, std::memory_order_release);
      }
    });
  }
  for (auto& t : ts) t.join();
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  Report("prodcon", std::to_string(pairs) + "pairs", secs, static_cast<std::uint64_t>(pairs) * items, base);
  return 0;
}

// Thread-local churn: allocate a batch of small objects, free them in reverse. Measures the
// fast path with no sharing at all.
int Churn(int threads, int rounds) {
  constexpr int kBatch = 256;
  const double base = PeakRssMb();
  const auto t0 = Clock::now();
  std::vector<std::thread> ts;
  for (int t = 0; t < threads; ++t) {
    ts.emplace_back([rounds, t] {
      void* batch[kBatch];
      std::mt19937 rng(static_cast<unsigned>(t));
      for (int r = 0; r < rounds; ++r) {
        for (int i = 0; i < kBatch; ++i) {
          batch[i] = backend::Malloc(8 + (rng() & 255));
          static_cast<volatile char*>(batch[i])[0] = 1;
        }
        for (int i = kBatch - 1; i >= 0; --i) backend::Free(batch[i]);
      }
    });
  }
  for (auto& t : ts) t.join();
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  Report("churn", std::to_string(threads) + "t", secs, static_cast<std::uint64_t>(threads) * rounds * kBatch * 2,
         base);
  return 0;
}

// Fragmentation: fill memory with small objects, free a random 90%, then allocate objects of
// a different size class. Reports peak RSS: allocators that cannot reuse or return the
// partially-emptied pages peak higher.
int Fragment(int millions) {
  const std::size_t n = static_cast<std::size_t>(millions) * 1000000;
  std::vector<void*> v(n);
  std::mt19937_64 rng(7);
  const double base = PeakRssMb();
  const auto t0 = Clock::now();
  for (auto& p : v) {
    p = backend::Malloc(64);
    static_cast<volatile char*>(p)[0] = 1;
  }
  for (auto& p : v) {
    if (rng() % 10 != 0) {
      backend::Free(p);
      p = nullptr;
    }
  }
  std::vector<void*> w(n / 2);
  for (auto& p : w) {
    p = backend::Malloc(200);
    static_cast<volatile char*>(p)[0] = 1;
  }
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  for (void* p : v) backend::Free(p);
  for (void* p : w) backend::Free(p);
  Report("fragment", std::to_string(millions) + "M", secs, n + n / 2, base);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s replay <trace.hwt> | larson|prodcon|churn <threads> <n> | fragment <M>\n",
                 argv[0]);
    return 2;
  }
  const std::string w = argv[1];
  if (w == "replay") return Replay(argv[2]);
  if (w == "fragment") return Fragment(std::atoi(argv[2]));
  if (argc < 4) return 2;
  const int a = std::atoi(argv[2]);
  const int b = std::atoi(argv[3]);
  if (w == "larson") return Larson(a, b);
  if (w == "prodcon") return ProdCon(a, b);
  if (w == "churn") return Churn(a, b);
  std::fprintf(stderr, "unknown workload %s\n", w.c_str());
  return 2;
}
