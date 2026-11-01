// Internal layout of heapwise. See docs/DESIGN.md for the full picture.
#ifndef HEAPWISE_INTERNAL_H
#define HEAPWISE_INTERNAL_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#define HW_LIKELY(x) __builtin_expect(!!(x), 1)
#define HW_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define HW_NOINLINE __attribute__((noinline))

namespace hw {

constexpr std::size_t kSegmentShift = 22;  // 4 MiB segments
constexpr std::size_t kSegmentSize = std::size_t{1} << kSegmentShift;
constexpr std::uintptr_t kSegmentMask = kSegmentSize - 1;
constexpr std::size_t kUnitShift = 16;  // 64 KiB units
constexpr std::size_t kUnitSize = std::size_t{1} << kUnitShift;
constexpr std::size_t kUnitsPerSegment = kSegmentSize / kUnitSize;  // 64
constexpr std::size_t kNumClasses = 56;
constexpr std::size_t kMaxSmallSize = 262144;  // largest size class
constexpr std::size_t kMaxAlign = kSegmentSize / 2;
constexpr std::size_t kMinAlign = 16;
constexpr std::size_t kCacheLine = 128;  // Apple M-series reports 128 bytes
constexpr std::size_t kHugeHeader = kUnitSize;  // huge object starts 64 KiB into its mapping
constexpr std::uint64_t kAllUsableUnits = ~std::uint64_t{1};  // unit 0 holds the header

static_assert(kUnitsPerSegment == 64, "free-unit bitmap is a single uint64_t");

// --- size classes --------------------------------------------------------
// Classes 0..15: 16..256 in steps of 16. Above 256: four classes per power of two.
constexpr std::uint32_t SizeClass(std::size_t n) {
  if (n <= 256) return n == 0 ? 0 : static_cast<std::uint32_t>((n + 15) >> 4) - 1;
  const std::size_t s = n - 1;
  const auto b = static_cast<std::uint32_t>(63 - __builtin_clzll(s));
  return 16 + (b - 8) * 4 + static_cast<std::uint32_t>((s >> (b - 2)) & 3);
}

constexpr std::size_t ClassSize(std::uint32_t c) {
  if (c < 16) return (std::size_t{c} + 1) * 16;
  const std::uint32_t b = 8 + (c - 16) / 4;
  const std::uint32_t q = (c - 16) % 4;
  return (std::size_t{1} << b) + (std::size_t{q} + 1) * (std::size_t{1} << (b - 2));
}

// Units per page for a class: enough room for at least 8 blocks, capped at 32 units.
constexpr std::uint32_t ClassUnits(std::uint32_t c) {
  const std::size_t want = (ClassSize(c) * 8 + kUnitSize - 1) / kUnitSize;
  return static_cast<std::uint32_t>(want < 1 ? 1 : (want > 32 ? 32 : want));
}

static_assert(ClassSize(kNumClasses - 1) == kMaxSmallSize);
static_assert(SizeClass(kMaxSmallSize) == kNumClasses - 1);

struct Block {
  Block* next;
};

struct Heap;
struct Segment;

enum class PageKind : std::uint8_t { kFree = 0, kSmall = 1, kTail = 2 };

// Per-unit metadata. Only the head unit of a span carries meaningful state;
// every unit stores the index of its span head so lookup is branch-free.
// All fields except `remote` (kept in Segment::remote) are touched only by the owning heap.
struct Page {
  Block* free;          // owner-local free list
  Page* next;           // links in the heap's class queue or full list
  Page* prev;
  std::uint8_t* start;  // first block
  std::uint32_t block_size;
  std::uint16_t used;      // blocks handed out and not yet returned to `free`
  std::uint16_t capacity;  // blocks carved so far (lazy bump extension)
  std::uint16_t reserved;  // blocks that fit in the span
  std::uint8_t units;
  std::uint8_t head;  // index of the span head unit
  PageKind kind;
  std::uint8_t size_class;
  bool in_full;
};

// Cross-thread frees land here, one cache line per unit so remote frees do not
// false-share with the owner's metadata.
struct alignas(kCacheLine) RemoteSlot {
  std::atomic<Block*> head;
};

enum class SegmentKind : std::uint32_t { kNormal = 0, kHuge = 1 };

struct alignas(kCacheLine) Segment {
  std::atomic<Heap*> owner;  // nullptr when abandoned or huge
  Segment* next;             // heap lists, abandoned list, or cache
  Segment* prev;
  std::uint64_t free_units;  // bit i set => unit i free
  std::size_t mapped_size;
  SegmentKind kind;
  bool in_full_list;
  std::array<Page, kUnitsPerSegment> pages;
  std::array<RemoteSlot, kUnitsPerSegment> remote;
};

static_assert(sizeof(Segment) <= kUnitSize, "segment header must fit in unit 0");

struct Heap {
  std::array<Page*, kNumClasses> pages;  // non-full pages, head is current
  std::array<Page*, kNumClasses> full;   // pages with no free or carvable blocks
  std::array<std::uint32_t, kNumClasses> full_len;
  std::array<std::uint32_t, kNumClasses> new_since_scan;
  Segment* avail;     // segments with at least one free unit
  Segment* full_segs; // segments with no free units
  Segment* cache;     // one empty segment kept to avoid mmap churn
  Heap* next_free;    // global heap pool
};

inline Segment* SegmentOf(const void* p) {
  return reinterpret_cast<Segment*>(reinterpret_cast<std::uintptr_t>(p) & ~kSegmentMask);
}

inline std::size_t UnitOf(const Segment* s, const void* p) {
  return (reinterpret_cast<std::uintptr_t>(p) - reinterpret_cast<std::uintptr_t>(s)) >> kUnitShift;
}

// --- OS layer (os.cpp) ---------------------------------------------------
void* OsMapAligned(std::size_t size, std::size_t align);
void OsUnmap(void* p, std::size_t size);
void OsPurge(void* p, std::size_t size);    // tell the OS the pages are unused
void OsReuse(void* p, std::size_t size);    // undo OsPurge accounting (macOS)
void OsAdviseHuge(void* p, std::size_t size);

// Global bitmap of live segment base addresses (1 bit per 4 MiB of address space).
bool SegmapInit();
void SegmapSet(const void* seg, bool live);
bool SegmapContains(const void* seg);

struct OsCounters {
  std::atomic<std::uint64_t> maps{0};
  std::atomic<std::uint64_t> unmaps{0};
  std::atomic<std::uint64_t> bytes{0};
};
OsCounters& Counters();

}  // namespace hw

#endif  // HEAPWISE_INTERNAL_H
