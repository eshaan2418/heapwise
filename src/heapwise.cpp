// heapwise core: thread heaps, pages, segments, cross-thread frees, huge objects.
#include "heapwise/heapwise.h"

#include <pthread.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#include "internal.h"

#if defined(__linux__)
#define HW_TLS_MODEL __attribute__((tls_model("initial-exec")))
#else
#define HW_TLS_MODEL
#endif

namespace hw {
namespace {

thread_local Heap* tl_heap HW_TLS_MODEL = nullptr;

pthread_once_t g_once = PTHREAD_ONCE_INIT;
pthread_key_t g_key;
bool g_init_ok = false;

// On Apple arm64, pthread keys live in a per-thread array addressed by TPIDRRO_EL0; this is
// how libpthread's inline _pthread_getspecific_direct works. Reading our slot directly avoids
// the dyld TLV thunk that every `thread_local` access goes through on macOS. The layout is
// verified at startup (see GlobalInit) and we fall back to thread_local if the check fails.
// -1 until verified; a single atomic word so a reader never sees a torn or stale key.
std::atomic<long> g_tsd_slot{-1};

#if defined(__APPLE__) && defined(__aarch64__)
#define HW_DIRECT_TSD 1
inline void** TsdBase() {
  std::uintptr_t t = 0;
  __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(t));
  return reinterpret_cast<void**>(t);
}
#else
#define HW_DIRECT_TSD 0
#endif

inline Heap* CurrentHeap() {
#if HW_DIRECT_TSD
  const long slot = g_tsd_slot.load(std::memory_order_relaxed);
  if (HW_LIKELY(slot >= 0)) return static_cast<Heap*>(TsdBase()[slot]);
#endif
  return tl_heap;
}

std::mutex g_heap_mu;
Heap* g_heap_pool = nullptr;

std::mutex g_abandon_mu;
Segment* g_abandoned = nullptr;
std::atomic<std::uint64_t> g_abandoned_count{0};
std::atomic<std::uint64_t> g_segments{0};
std::atomic<std::uint64_t> g_huge{0};

constexpr std::size_t kHugeCacheSlots = 16;
constexpr std::size_t kHugeCacheMaxBytes = std::size_t{256} << 20;
constexpr std::size_t kHugeCacheMaxEntry = std::size_t{64} << 20;
constexpr std::size_t kOsPage = 16384;

struct HugeCache {
  std::mutex mu;
  std::array<Segment*, kHugeCacheSlots> items{};
  std::size_t count = 0;
  std::size_t bytes = 0;
};
HugeCache g_huge_cache;

[[noreturn]] void Fatal(const char* msg) {
  const char prefix[] = "heapwise: ";
  (void)!write(2, prefix, sizeof(prefix) - 1);
  (void)!write(2, msg, std::strlen(msg));
  (void)!write(2, "\n", 1);
  std::abort();
}

// --- intrusive lists -------------------------------------------------------

template <typename T>
void ListPush(T*& head, T* x) {
  x->prev = nullptr;
  x->next = head;
  if (head != nullptr) head->prev = x;
  head = x;
}

template <typename T>
void ListRemove(T*& head, T* x) {
  if (x->prev != nullptr) {
    x->prev->next = x->next;
  } else {
    head = x->next;
  }
  if (x->next != nullptr) x->next->prev = x->prev;
  x->next = nullptr;
  x->prev = nullptr;
}

constexpr std::uint64_t UnitMask(std::uint32_t first, std::uint32_t units) {
  return (units >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << units) - 1)) << first;
}

// --- heap pool ---------------------------------------------------------------

void ThreadExit(void* value);

void GlobalInit() {
  g_init_ok = SegmapInit() && pthread_key_create(&g_key, ThreadExit) == 0;
#if HW_DIRECT_TSD
  if (g_init_ok) {
    int probe = 0;
    pthread_setspecific(g_key, &probe);
    const bool layout_ok = TsdBase()[g_key] == &probe;
    pthread_setspecific(g_key, nullptr);
    if (layout_ok) g_tsd_slot.store(static_cast<long>(g_key), std::memory_order_relaxed);
  }
#endif
}

Heap* HeapNew() {
  std::lock_guard<std::mutex> lock(g_heap_mu);
  if (g_heap_pool == nullptr) {
    constexpr std::size_t kChunk = std::size_t{256} * 1024;
    void* mem = OsMapAligned(kChunk, kOsPage);
    if (mem == nullptr) return nullptr;
    auto* heaps = static_cast<Heap*>(mem);
    for (std::size_t i = 0; i < kChunk / sizeof(Heap); ++i) {
      heaps[i].next_free = g_heap_pool;
      g_heap_pool = &heaps[i];
    }
  }
  Heap* h = g_heap_pool;
  g_heap_pool = h->next_free;
  *h = Heap{};
  return h;
}

void HeapRelease(Heap* h) {
  std::lock_guard<std::mutex> lock(g_heap_mu);
  h->next_free = g_heap_pool;
  g_heap_pool = h;
}

HW_NOINLINE Heap* HeapInit() {
  pthread_once(&g_once, GlobalInit);
  if (!g_init_ok) return nullptr;
  Heap* h = HeapNew();
  if (h == nullptr) return nullptr;
  tl_heap = h;
  pthread_setspecific(g_key, h);
  return h;
}

inline Heap* GetHeap() {
  Heap* h = CurrentHeap();
  return HW_LIKELY(h != nullptr) ? h : HeapInit();
}

// --- segments ----------------------------------------------------------------

void SegmentDestroy(Segment* s) {
  SegmapSet(s, false);
  if (s->kind == SegmentKind::kHuge) {
    g_huge.fetch_sub(1, std::memory_order_relaxed);
  } else {
    g_segments.fetch_sub(1, std::memory_order_relaxed);
  }
  OsUnmap(s, s->mapped_size);
}

Segment* SegmentFromOs(Heap* h) {
  void* mem = OsMapAligned(kSegmentSize, kSegmentSize);
  if (mem == nullptr) return nullptr;
  auto* s = new (mem) Segment;  // fresh mmap memory is zero; the ctor only sets the atomics
  s->owner.store(h, std::memory_order_relaxed);
  s->free_units = kAllUsableUnits;
  s->mapped_size = kSegmentSize;
  s->kind = SegmentKind::kNormal;
  SegmapSet(s, true);
  OsAdviseHuge(s, kSegmentSize);
  g_segments.fetch_add(1, std::memory_order_relaxed);
  return s;
}

void HeapTrackSegment(Heap* h, Segment* s) {
  s->in_full_list = s->free_units == 0;
  ListPush(s->in_full_list ? h->full_segs : h->avail, s);
}

void HeapUntrackSegment(Heap* h, Segment* s) { ListRemove(s->in_full_list ? h->full_segs : h->avail, s); }

// Called when every unit of `s` is free.
void SegmentRelease(Heap* h, Segment* s) {
  HeapUntrackSegment(h, s);
  if (h->cache == nullptr) {
    OsPurge(reinterpret_cast<std::uint8_t*>(s) + kUnitSize, kSegmentSize - kUnitSize);
    h->cache = s;
    return;
  }
  SegmentDestroy(s);
}

Page* SegmentAllocSpan(Heap* h, Segment* s, std::uint32_t units, std::uint32_t cls) {
  std::uint64_t fit = s->free_units;
  for (std::uint32_t i = 1; i < units && fit != 0; ++i) fit &= s->free_units >> i;
  if (fit == 0) return nullptr;
  const auto u = static_cast<std::uint32_t>(__builtin_ctzll(fit));
  s->free_units &= ~UnitMask(u, units);
  if (s->free_units == 0) {
    ListRemove(h->avail, s);
    s->in_full_list = true;
    ListPush(h->full_segs, s);
  }
  for (std::uint32_t k = 0; k < units; ++k) {
    Page& t = s->pages[u + k];
    t.kind = PageKind::kTail;
    t.head = static_cast<std::uint8_t>(u);
  }
  Page* p = &s->pages[u];
  const std::size_t bs = ClassSize(cls);
  p->kind = PageKind::kSmall;
  p->units = static_cast<std::uint8_t>(units);
  p->size_class = static_cast<std::uint8_t>(cls);
  p->block_size = static_cast<std::uint32_t>(bs);
  p->start = reinterpret_cast<std::uint8_t*>(s) + (std::size_t{u} << kUnitShift);
  p->reserved = static_cast<std::uint16_t>((std::size_t{units} << kUnitShift) / bs);
  p->capacity = 0;
  p->used = 0;
  p->free = nullptr;
  p->next = nullptr;
  p->prev = nullptr;
  p->in_full = false;
  return p;
}

// Returns true if the whole segment was released as a result.
bool SegmentFreeSpan(Heap* h, Segment* s, Page* p) {
  const std::uint32_t u = p->head;
  const std::uint32_t units = p->units;
  for (std::uint32_t k = 0; k < units; ++k) s->pages[u + k].kind = PageKind::kFree;
  const bool was_full = s->free_units == 0;
  s->free_units |= UnitMask(u, units);
  if (was_full) {
    ListRemove(h->full_segs, s);
    s->in_full_list = false;
    ListPush(h->avail, s);
  }
  if (s->free_units == kAllUsableUnits) {
    SegmentRelease(h, s);
    return true;
  }
  return false;
}

// --- pages -------------------------------------------------------------------

inline void PageCollect(Segment* s, Page* p) {
  std::atomic<Block*>& remote = s->remote[p->head].head;
  if (remote.load(std::memory_order_relaxed) == nullptr) return;
  Block* list = remote.exchange(nullptr, std::memory_order_acquire);
  std::uint32_t n = 1;
  Block* tail = list;
  while (tail->next != nullptr) {
    tail = tail->next;
    ++n;
  }
  tail->next = p->free;
  p->free = list;
  p->used = static_cast<std::uint16_t>(p->used - n);
}

// Carve more blocks from the untouched tail of the span, roughly one OS page at a time,
// so a fresh page does not commit memory it never uses.
void PageExtend(Page* p) {
  if (p->capacity >= p->reserved) return;
  const std::size_t bs = p->block_size;
  std::size_t n = bs >= 4096 ? 1 : 4096 / bs;
  const std::size_t left = std::size_t{p->reserved} - p->capacity;
  if (n > left) n = left;
  std::uint8_t* first = p->start + std::size_t{p->capacity} * bs;
  for (std::size_t i = 0; i + 1 < n; ++i) {
    reinterpret_cast<Block*>(first + i * bs)->next = reinterpret_cast<Block*>(first + (i + 1) * bs);
  }
  reinterpret_cast<Block*>(first + (n - 1) * bs)->next = p->free;
  p->free = reinterpret_cast<Block*>(first);
  p->capacity = static_cast<std::uint16_t>(p->capacity + n);
}

inline void* PagePop(Page* p) {
  Block* b = p->free;
  p->free = b->next;
  ++p->used;
  return b;
}

void MoveToFull(Heap* h, Page* p) {
  const std::uint32_t c = p->size_class;
  ListRemove(h->pages[c], p);
  p->in_full = true;
  ListPush(h->full[c], p);
  ++h->full_len[c];
}

void MoveFromFull(Heap* h, Page* p) {
  const std::uint32_t c = p->size_class;
  ListRemove(h->full[c], p);
  --h->full_len[c];
  p->in_full = false;
  ListPush(h->pages[c], p);
}

// Rescan full pages for cross-thread frees. A scan is triggered once the heap has allocated
// as many fresh pages as the previous scan failed to reclaim, so a scan's O(full pages) cost
// is paid for by an equal number of page allocations: amortised O(1) per page, and pages
// emptied by remote frees are found promptly when most of them do get reclaimed.
void ScanFull(Heap* h, std::uint32_t c) {
  Page* p = h->full[c];
  while (p != nullptr) {
    Page* next = p->next;
    Segment* s = SegmentOf(p);
    PageCollect(s, p);
    if (p->free != nullptr) {
      MoveFromFull(h, p);
      // Keep one empty page per class; return the rest of the emptied pages to their segment.
      if (p->used == 0 && p->next != nullptr) {
        ListRemove(h->pages[c], p);
        SegmentFreeSpan(h, s, p);
      }
    }
    p = next;
  }
  h->new_since_scan[c] = 0;
  h->scan_budget[c] = h->full_len[c];
}

// Take ownership of an abandoned segment and enqueue its usable pages.
// Returns false if the segment turned out to be completely free and was released.
bool Adopt(Heap* h, Segment* s) {
  s->owner.store(h, std::memory_order_relaxed);
  s->next = nullptr;
  s->prev = nullptr;
  HeapTrackSegment(h, s);
  std::uint32_t u = 1;
  while (u < kUnitsPerSegment) {
    Page* p = &s->pages[u];
    if (p->kind != PageKind::kSmall || p->head != u) {
      ++u;
      continue;
    }
    const std::uint32_t units = p->units;
    PageCollect(s, p);
    if (p->used == 0) {
      if (SegmentFreeSpan(h, s, p)) return false;
    } else {
      p->in_full = false;
      p->next = nullptr;
      p->prev = nullptr;
      const std::uint32_t c = p->size_class;
      if (p->free != nullptr || p->capacity < p->reserved) {
        ListPush(h->pages[c], p);
      } else {
        p->in_full = true;
        ListPush(h->full[c], p);
        ++h->full_len[c];
      }
    }
    u += units;
  }
  return true;
}

Segment* PopAbandoned() {
  if (g_abandoned_count.load(std::memory_order_relaxed) == 0) return nullptr;
  std::lock_guard<std::mutex> lock(g_abandon_mu);
  Segment* s = g_abandoned;
  if (s != nullptr) {
    ListRemove(g_abandoned, s);
    s->abandoned = false;
    g_abandoned_count.fetch_sub(1, std::memory_order_relaxed);
  }
  return s;
}

// A thread freeing into an abandoned segment takes it over, so the rest of its frees into
// that segment are local (no atomics) and empty pages can be released immediately.
// The caller holds a live block in `s`, so `s` cannot be unmapped underneath us.
HW_NOINLINE bool TryAdoptOnFree(Heap* h, Segment* s) {
  {
    std::lock_guard<std::mutex> lock(g_abandon_mu);
    if (!s->abandoned) return false;
    ListRemove(g_abandoned, s);
    s->abandoned = false;
    g_abandoned_count.fetch_sub(1, std::memory_order_relaxed);
  }
  return Adopt(h, s);
}

Segment* SegmentAcquire(Heap* h) {
  if (h->cache != nullptr) {
    Segment* s = h->cache;
    h->cache = nullptr;
    OsReuse(reinterpret_cast<std::uint8_t*>(s) + kUnitSize, kSegmentSize - kUnitSize);
    HeapTrackSegment(h, s);
    return s;
  }
  while (Segment* s = PopAbandoned()) {
    if (Adopt(h, s)) return s;
    if (h->cache != nullptr) return SegmentAcquire(h);
  }
  Segment* s = SegmentFromOs(h);
  if (s != nullptr) HeapTrackSegment(h, s);
  return s;
}

Page* PageAlloc(Heap* h, std::uint32_t c) {
  const std::uint32_t units = ClassUnits(c);
  int tries = 0;
  for (Segment* s = h->avail; s != nullptr && tries < 4; s = s->next, ++tries) {
    if (Page* p = SegmentAllocSpan(h, s, units, c)) return p;
  }
  for (;;) {
    Segment* s = SegmentAcquire(h);
    if (s == nullptr) return nullptr;
    if (Page* p = SegmentAllocSpan(h, s, units, c)) return p;
  }
}

HW_NOINLINE void* MallocSlow(Heap* h, std::uint32_t c) {
  for (int round = 0; round < 2; ++round) {
    Page* p = h->pages[c];
    while (p != nullptr) {
      PageCollect(SegmentOf(p), p);
      if (p->free == nullptr) PageExtend(p);
      if (p->free != nullptr) {
        if (p != h->pages[c]) {
          ListRemove(h->pages[c], p);
          ListPush(h->pages[c], p);
        }
        return PagePop(p);
      }
      Page* next = p->next;
      MoveToFull(h, p);
      p = next;
    }
    if (round == 0 && h->full_len[c] > 0 && h->new_since_scan[c] >= h->scan_budget[c]) {
      ScanFull(h, c);
      continue;
    }
    break;
  }
  Page* p = PageAlloc(h, c);
  if (p == nullptr) {
    errno = ENOMEM;
    return nullptr;
  }
  ++h->new_since_scan[c];
  ListPush(h->pages[c], p);
  PageExtend(p);
  return PagePop(p);
}

inline void* MallocClass(std::uint32_t c) {
  Heap* h = GetHeap();
  if (HW_UNLIKELY(h == nullptr)) {
    errno = ENOMEM;
    return nullptr;
  }
  Page* p = h->pages[c];
  if (HW_LIKELY(p != nullptr && p->free != nullptr)) return PagePop(p);
  return MallocSlow(h, c);
}

// --- huge objects --------------------------------------------------------------

Segment* HugeCacheTake(std::size_t need) {
  std::lock_guard<std::mutex> lock(g_huge_cache.mu);
  for (std::size_t i = 0; i < g_huge_cache.count; ++i) {
    Segment* s = g_huge_cache.items[i];
    if (s->mapped_size >= need && s->mapped_size - need <= need / 2) {
      g_huge_cache.items[i] = g_huge_cache.items[--g_huge_cache.count];
      g_huge_cache.bytes -= s->mapped_size;
      return s;
    }
  }
  return nullptr;
}

bool HugeCachePut(Segment* s) {
  if (s->mapped_size > kHugeCacheMaxEntry) return false;
  std::lock_guard<std::mutex> lock(g_huge_cache.mu);
  if (g_huge_cache.count == kHugeCacheSlots || g_huge_cache.bytes + s->mapped_size > kHugeCacheMaxBytes) return false;
  OsPurge(reinterpret_cast<std::uint8_t*>(s) + kHugeHeader, s->mapped_size - kHugeHeader);
  g_huge_cache.items[g_huge_cache.count++] = s;
  g_huge_cache.bytes += s->mapped_size;
  return true;
}

void HugeCacheClear() {
  std::array<Segment*, kHugeCacheSlots> drop{};
  std::size_t n = 0;
  {
    std::lock_guard<std::mutex> lock(g_huge_cache.mu);
    n = g_huge_cache.count;
    drop = g_huge_cache.items;
    g_huge_cache.count = 0;
    g_huge_cache.bytes = 0;
  }
  for (std::size_t i = 0; i < n; ++i) SegmentDestroy(drop[i]);
}

void* HugeAlloc(std::size_t size, std::size_t align, bool* zeroed) {
  pthread_once(&g_once, GlobalInit);
  if (!g_init_ok || size > (SIZE_MAX >> 2)) {
    errno = ENOMEM;
    return nullptr;
  }
  const std::size_t offset = align > kHugeHeader ? align : kHugeHeader;
  const std::size_t need = (offset + size + kOsPage - 1) & ~(kOsPage - 1);
  Segment* s = HugeCacheTake(need);
  if (s != nullptr) {
    OsReuse(reinterpret_cast<std::uint8_t*>(s) + kHugeHeader, s->mapped_size - kHugeHeader);
    if (zeroed != nullptr) *zeroed = false;
  } else {
    void* mem = OsMapAligned(need, kSegmentSize);
    if (mem == nullptr) {
      errno = ENOMEM;
      return nullptr;
    }
    s = new (mem) Segment;
    s->kind = SegmentKind::kHuge;
    s->mapped_size = need;
    SegmapSet(s, true);
    g_huge.fetch_add(1, std::memory_order_relaxed);
    if (zeroed != nullptr) *zeroed = true;
  }
  return reinterpret_cast<std::uint8_t*>(s) + offset;
}

// --- free ----------------------------------------------------------------------

HW_NOINLINE void FreeLocalSlow(Heap* h, Segment* s, Page* p) {
  const std::uint32_t c = p->size_class;
  if (p->in_full) MoveFromFull(h, p);
  if (p->used == 0 && !(h->pages[c] == p && p->next == nullptr)) {
    ListRemove(h->pages[c], p);
    SegmentFreeSpan(h, s, p);
  }
}

inline Segment* CheckedSegment(const void* ptr) {
  Segment* s = SegmentOf(ptr);
  if (HW_UNLIKELY(!SegmapContains(s))) return nullptr;
  return s;
}

void ThreadExit(void* value) {
  auto* h = static_cast<Heap*>(value);
  // Detach every segment, free empty spans, hand the rest to the abandoned list.
  Segment* lists[2] = {h->avail, h->full_segs};
  for (Segment* s : lists) {
    while (s != nullptr) {
      Segment* next = s->next;
      for (std::uint32_t u = 1; u < kUnitsPerSegment;) {
        Page* p = &s->pages[u];
        if (p->kind != PageKind::kSmall || p->head != u) {
          ++u;
          continue;
        }
        const std::uint32_t units = p->units;
        PageCollect(s, p);
        if (p->used == 0) {
          for (std::uint32_t k = 0; k < units; ++k) s->pages[u + k].kind = PageKind::kFree;
          s->free_units |= UnitMask(u, units);
        }
        u += units;
      }
      if (s->free_units == kAllUsableUnits) {
        SegmentDestroy(s);
      } else {
        s->owner.store(nullptr, std::memory_order_release);
        std::lock_guard<std::mutex> lock(g_abandon_mu);
        ListPush(g_abandoned, s);
        s->abandoned = true;
        g_abandoned_count.fetch_add(1, std::memory_order_relaxed);
      }
      s = next;
    }
  }
  if (h->cache != nullptr) SegmentDestroy(h->cache);
  tl_heap = nullptr;
  HeapRelease(h);
}

}  // namespace
}  // namespace hw

using hw::Block;
using hw::Heap;
using hw::Page;
using hw::Segment;

extern "C" {

void* hw_malloc(size_t size) {
  if (HW_LIKELY(size <= hw::kMaxSmallSize)) return hw::MallocClass(hw::SizeClass(size));
  return hw::HugeAlloc(size, hw::kMinAlign, nullptr);
}

void hw_free(void* ptr) {
  if (ptr == nullptr) return;
  Segment* s = hw::CheckedSegment(ptr);
  if (HW_UNLIKELY(s == nullptr)) {
#if defined(HW_OVERRIDE)
    return;  // memory from before the override took effect: leak rather than crash
#else
    hw::Fatal("hw_free: pointer was not allocated by heapwise");
#endif
  }
  if (HW_UNLIKELY(s->kind == hw::SegmentKind::kHuge)) {
    if (!hw::HugeCachePut(s)) hw::SegmentDestroy(s);
    return;
  }
  Page* p = &s->pages[s->pages[hw::UnitOf(s, ptr)].head];
  auto* b = static_cast<Block*>(ptr);
  Heap* h = hw::CurrentHeap();
  Heap* owner = s->owner.load(std::memory_order_relaxed);
  if (HW_UNLIKELY(owner == nullptr && h != nullptr) && hw::TryAdoptOnFree(h, s)) owner = h;
  if (HW_LIKELY(h != nullptr && owner == h)) {
    b->next = p->free;
    p->free = b;
    --p->used;
    // A full page rejoins the allocation queue only once 1/8 of its blocks are free.
    // Without this hysteresis, a single free into a full page moves it to the queue head, the
    // next malloc drains it, and it bounces straight back to the full list.
    if (HW_UNLIKELY(p->used == 0 ||
                    (p->in_full && static_cast<std::uint32_t>(p->capacity - p->used) >= (p->reserved >> 3u)))) {
      hw::FreeLocalSlow(h, s, p);
    }
    return;
  }
  // Cross-thread free: push onto the page's lock-free stack. The owner collects the whole
  // stack with a single exchange, so there is no pop and no ABA hazard.
  std::atomic<Block*>& remote = s->remote[p->head].head;
  Block* head = remote.load(std::memory_order_relaxed);
  do {
    b->next = head;
  } while (!remote.compare_exchange_weak(head, b, std::memory_order_release, std::memory_order_relaxed));
}

void* hw_calloc(size_t count, size_t size) {
  size_t total = 0;
  if (__builtin_mul_overflow(count, size, &total)) {
    errno = ENOMEM;
    return nullptr;
  }
  if (total > hw::kMaxSmallSize) {
    bool zeroed = false;
    void* p = hw::HugeAlloc(total, hw::kMinAlign, &zeroed);
    if (p != nullptr && !zeroed) std::memset(p, 0, total);
    return p;
  }
  void* p = hw_malloc(total);
  if (p != nullptr) std::memset(p, 0, total);
  return p;
}

size_t hw_usable_size(const void* ptr) {
  if (ptr == nullptr) return 0;
  Segment* s = hw::CheckedSegment(ptr);
  if (s == nullptr) hw::Fatal("hw_usable_size: pointer was not allocated by heapwise");
  if (s->kind == hw::SegmentKind::kHuge) {
    return reinterpret_cast<std::uintptr_t>(s) + s->mapped_size - reinterpret_cast<std::uintptr_t>(ptr);
  }
  return s->pages[s->pages[hw::UnitOf(s, ptr)].head].block_size;
}

void* hw_realloc(void* ptr, size_t size) {
  if (ptr == nullptr) return hw_malloc(size);
  if (size == 0) size = 1;
  const size_t usable = hw_usable_size(ptr);
  if (size <= usable && size >= usable / 2) return ptr;
  void* q = hw_malloc(size);
  if (q == nullptr) return nullptr;
  std::memcpy(q, ptr, size < usable ? size : usable);
  hw_free(ptr);
  return q;
}

void* hw_aligned_alloc(size_t alignment, size_t size) {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment > hw::kMaxAlign) {
    errno = EINVAL;
    return nullptr;
  }
  if (alignment <= hw::kMinAlign) return hw_malloc(size);
  if (size <= hw::kMaxSmallSize && alignment <= hw::kUnitSize) {
    // Pages start on 64 KiB boundaries, so a class whose size is a multiple of the
    // alignment yields aligned blocks. Power-of-two classes always qualify.
    std::uint32_t c = hw::SizeClass(size < alignment ? alignment : size);
    while (c < hw::kNumClasses && hw::ClassSize(c) % alignment != 0) ++c;
    if (c < hw::kNumClasses) return hw::MallocClass(c);
  }
  return hw::HugeAlloc(size, alignment, nullptr);
}

int hw_posix_memalign(void** out, size_t alignment, size_t size) {
  if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0 || alignment > hw::kMaxAlign) return EINVAL;
  void* p = hw_aligned_alloc(alignment, size);
  if (p == nullptr) return ENOMEM;
  *out = p;
  return 0;
}

int hw_owns(const void* ptr) { return ptr != nullptr && hw::SegmapContains(hw::SegmentOf(ptr)) ? 1 : 0; }

void hw_collect(int force) {
  Heap* h = force ? hw::GetHeap() : hw::CurrentHeap();
  if (h != nullptr) {
    if (force) {
      while (Segment* s = hw::PopAbandoned()) hw::Adopt(h, s);
    }
    for (std::uint32_t c = 0; c < hw::kNumClasses; ++c) {
      hw::ScanFull(h, c);
      Page* p = h->pages[c];
      while (p != nullptr) {
        Page* next = p->next;
        Segment* s = hw::SegmentOf(p);
        hw::PageCollect(s, p);
        if (p->used == 0) {
          hw::ListRemove(h->pages[c], p);
          hw::SegmentFreeSpan(h, s, p);
        }
        p = next;
      }
    }
    if (h->cache != nullptr) {
      hw::SegmentDestroy(h->cache);
      h->cache = nullptr;
    }
  }
  hw::HugeCacheClear();
}

void hw_get_stats(hw_stats* out) {
  out->segments_mapped = hw::g_segments.load(std::memory_order_relaxed);
  out->huge_mapped = hw::g_huge.load(std::memory_order_relaxed);
  out->bytes_mapped = hw::Counters().bytes.load(std::memory_order_relaxed);
  out->segments_abandoned = hw::g_abandoned_count.load(std::memory_order_relaxed);
  out->os_maps = hw::Counters().maps.load(std::memory_order_relaxed);
  out->os_unmaps = hw::Counters().unmaps.load(std::memory_order_relaxed);
}

}  // extern "C"
