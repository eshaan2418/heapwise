#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>

#include "internal.h"

namespace hw {

namespace {

constexpr int kAddressBits = 48;
constexpr std::size_t kSegmapBits = std::size_t{1} << (kAddressBits - kSegmentShift);
constexpr std::size_t kSegmapWords = kSegmapBits / 64;

std::atomic<std::atomic<std::uint64_t>*> g_segmap{nullptr};
OsCounters g_counters;

void* RawMap(std::size_t size) {
  void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  g_counters.maps.fetch_add(1, std::memory_order_relaxed);
  return p;
}

void RawUnmap(void* p, std::size_t size) {
  if (size == 0) return;
  munmap(p, size);
  g_counters.unmaps.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

OsCounters& Counters() { return g_counters; }

void* OsMapAligned(std::size_t size, std::size_t align) {
  // Fast path: the kernel often hands back aligned regions when we ask for multiples.
  void* p = RawMap(size);
  if (p == nullptr) return nullptr;
  auto addr = reinterpret_cast<std::uintptr_t>(p);
  if ((addr & (align - 1)) == 0) {
    g_counters.bytes.fetch_add(size, std::memory_order_relaxed);
    return p;
  }
  RawUnmap(p, size);
  // Over-allocate and trim both ends.
  const std::size_t over = size + align;
  p = RawMap(over);
  if (p == nullptr) return nullptr;
  addr = reinterpret_cast<std::uintptr_t>(p);
  const std::uintptr_t aligned = (addr + align - 1) & ~(align - 1);
  const std::size_t head = aligned - addr;
  const std::size_t tail = over - head - size;
  if (head != 0) RawUnmap(p, head);
  if (tail != 0) RawUnmap(reinterpret_cast<void*>(aligned + size), tail);
  g_counters.bytes.fetch_add(size, std::memory_order_relaxed);
  return reinterpret_cast<void*>(aligned);
}

void OsUnmap(void* p, std::size_t size) {
  RawUnmap(p, size);
  g_counters.bytes.fetch_sub(size, std::memory_order_relaxed);
}

void OsPurge(void* p, std::size_t size) {
#if defined(__APPLE__)
  madvise(p, size, MADV_FREE_REUSABLE);
#else
  madvise(p, size, MADV_DONTNEED);
#endif
}

void OsReuse(void* p, std::size_t size) {
#if defined(__APPLE__)
  madvise(p, size, MADV_FREE_REUSE);
#else
  (void)p;
  (void)size;
#endif
}

void OsAdviseHuge(void* p, std::size_t size) {
#if defined(__linux__) && defined(MADV_HUGEPAGE)
  static const bool enabled = [] {
    const char* v = std::getenv("HEAPWISE_HUGEPAGES");
    return v != nullptr && v[0] == '1';
  }();
  if (enabled) madvise(p, size, MADV_HUGEPAGE);
#else
  (void)p;
  (void)size;
#endif
}

bool SegmapInit() {
  if (g_segmap.load(std::memory_order_acquire) != nullptr) return true;
  // 8 MiB of virtual memory, committed lazily by the kernel as bits are touched.
  void* mem = RawMap(kSegmapWords * sizeof(std::uint64_t));
  if (mem == nullptr) return false;
  auto* words = static_cast<std::atomic<std::uint64_t>*>(mem);
  std::atomic<std::uint64_t>* expected = nullptr;
  if (!g_segmap.compare_exchange_strong(expected, words, std::memory_order_acq_rel)) {
    RawUnmap(mem, kSegmapWords * sizeof(std::uint64_t));
  }
  return true;
}

void SegmapSet(const void* seg, bool live) {
  const std::uintptr_t idx = reinterpret_cast<std::uintptr_t>(seg) >> kSegmentShift;
  if (idx >= kSegmapBits) std::abort();  // address space larger than we planned for
  auto* words = g_segmap.load(std::memory_order_acquire);
  const std::uint64_t bit = std::uint64_t{1} << (idx & 63);
  if (live) {
    words[idx >> 6].fetch_or(bit, std::memory_order_release);
  } else {
    words[idx >> 6].fetch_and(~bit, std::memory_order_release);
  }
}

bool SegmapContains(const void* seg) {
  const std::uintptr_t idx = reinterpret_cast<std::uintptr_t>(seg) >> kSegmentShift;
  if (idx >= kSegmapBits) return false;
  auto* words = g_segmap.load(std::memory_order_acquire);
  if (words == nullptr) return false;
  return (words[idx >> 6].load(std::memory_order_relaxed) >> (idx & 63)) & 1;
}

}  // namespace hw
