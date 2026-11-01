#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "heapwise/heapwise.h"

namespace {

bool Aligned(const void* p, std::size_t a) { return (reinterpret_cast<std::uintptr_t>(p) & (a - 1)) == 0; }

TEST(Basic, MallocFreeAllSizes) {
  std::vector<void*> ptrs;
  for (std::size_t n = 0; n <= 300000; n = n < 1024 ? n + 1 : n * 5 / 4) {
    void* p = hw_malloc(n);
    ASSERT_NE(p, nullptr) << n;
    EXPECT_TRUE(Aligned(p, 16)) << n;
    EXPECT_GE(hw_usable_size(p), n);
    EXPECT_TRUE(hw_owns(p));
    std::memset(p, 0xab, n);
    ptrs.push_back(p);
  }
  std::set<void*> unique(ptrs.begin(), ptrs.end());
  EXPECT_EQ(unique.size(), ptrs.size());
  for (void* p : ptrs) hw_free(p);
}

TEST(Basic, ZeroSizeIsUniqueAndFreeable) {
  void* a = hw_malloc(0);
  void* b = hw_malloc(0);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_NE(a, b);
  hw_free(a);
  hw_free(b);
  hw_free(nullptr);
}

TEST(Basic, FreedBlocksAreReused) {
  void* a = hw_malloc(48);
  hw_free(a);
  void* b = hw_malloc(48);
  EXPECT_EQ(a, b);
  hw_free(b);
}

TEST(Basic, CallocZeroesAndDetectsOverflow) {
  // Dirty a block, free it, then calloc the same class: must still come back zeroed.
  auto* d = static_cast<unsigned char*>(hw_malloc(512));
  std::memset(d, 0xff, 512);
  hw_free(d);
  auto* c = static_cast<unsigned char*>(hw_calloc(64, 8));
  ASSERT_NE(c, nullptr);
  for (int i = 0; i < 512; ++i) ASSERT_EQ(c[i], 0);
  hw_free(c);

  auto* big = static_cast<unsigned char*>(hw_calloc(1, 3 << 20));
  ASSERT_NE(big, nullptr);
  for (std::size_t i = 0; i < (3u << 20); i += 4093) ASSERT_EQ(big[i], 0);
  std::memset(big, 0xee, 3 << 20);
  hw_free(big);
  big = static_cast<unsigned char*>(hw_calloc(1, 3 << 20));  // likely from the huge cache
  for (std::size_t i = 0; i < (3u << 20); i += 4093) ASSERT_EQ(big[i], 0);
  hw_free(big);

  errno = 0;
  EXPECT_EQ(hw_calloc(SIZE_MAX / 2, 4), nullptr);
  EXPECT_EQ(errno, ENOMEM);
}

TEST(Basic, HugeMallocFailsCleanly) {
  errno = 0;
  EXPECT_EQ(hw_malloc(SIZE_MAX - 4096), nullptr);
  EXPECT_EQ(errno, ENOMEM);
}

TEST(Basic, ReallocPreservesContentsAcrossTiers) {
  const std::size_t sizes[] = {1, 24, 100, 4000, 70000, 262144, 262145, 5 << 20, 300, 8};
  auto* p = static_cast<unsigned char*>(hw_malloc(1));
  p[0] = 0;
  std::size_t prev = 1;
  for (std::size_t n : sizes) {
    p = static_cast<unsigned char*>(hw_realloc(p, n));
    ASSERT_NE(p, nullptr);
    const std::size_t keep = prev < n ? prev : n;
    for (std::size_t i = 0; i < keep; ++i) ASSERT_EQ(p[i], static_cast<unsigned char>(i * 7)) << n << " " << i;
    for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<unsigned char>(i * 7);
    prev = n;
  }
  hw_free(p);
  void* q = hw_realloc(nullptr, 40);
  ASSERT_NE(q, nullptr);
  void* r = hw_realloc(q, 0);
  ASSERT_NE(r, nullptr);
  hw_free(r);
}

TEST(Basic, ReallocInPlaceWithinClass) {
  void* p = hw_malloc(100);  // class 112
  EXPECT_EQ(hw_realloc(p, 110), p);
  hw_free(p);
}

TEST(Aligned, AllPowersOfTwo) {
  for (std::size_t a = 1; a <= (std::size_t{2} << 20); a <<= 1) {
    for (std::size_t n : {std::size_t{1}, std::size_t{100}, a, a + 1, std::size_t{300000}}) {
      void* p = hw_aligned_alloc(a, n);
      ASSERT_NE(p, nullptr) << a << " " << n;
      EXPECT_TRUE(Aligned(p, a)) << a << " " << n;
      EXPECT_GE(hw_usable_size(p), n);
      std::memset(p, 1, n);
      hw_free(p);
    }
  }
}

TEST(Aligned, RejectsBadAlignment) {
  errno = 0;
  EXPECT_EQ(hw_aligned_alloc(3, 10), nullptr);
  EXPECT_EQ(errno, EINVAL);
  EXPECT_EQ(hw_aligned_alloc(std::size_t{4} << 20, 10), nullptr);
  void* out = nullptr;
  EXPECT_EQ(hw_posix_memalign(&out, 4, 10), EINVAL);
  EXPECT_EQ(hw_posix_memalign(&out, 64, 10), 0);
  EXPECT_TRUE(Aligned(out, 64));
  hw_free(out);
}

TEST(Ownership, ForeignPointers) {
  int local = 0;
  EXPECT_FALSE(hw_owns(&local));
  EXPECT_FALSE(hw_owns(nullptr));
  void* sys = std::malloc(64);
  EXPECT_FALSE(hw_owns(sys));
  std::free(sys);
}

TEST(OwnershipDeathTest, FreeOfForeignPointerAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  int local = 0;
  EXPECT_DEATH(hw_free(&local), "not allocated by heapwise");
}

TEST(Reclaim, MemoryReturnsToOsAfterCollect) {
  hw_collect(1);
  hw_stats before{};
  hw_get_stats(&before);
  std::vector<void*> ptrs;
  for (int i = 0; i < 400000; ++i) ptrs.push_back(hw_malloc(static_cast<std::size_t>(64 + (i % 7) * 100)));
  hw_stats mid{};
  hw_get_stats(&mid);
  EXPECT_GT(mid.bytes_mapped, before.bytes_mapped + (100u << 20));
  for (void* p : ptrs) hw_free(p);
  hw_collect(0);
  hw_stats after{};
  hw_get_stats(&after);
  EXPECT_LE(after.bytes_mapped, before.bytes_mapped + (8u << 20));
}

TEST(Huge, CacheReusesMappings) {
  hw_collect(0);
  void* a = hw_malloc(1 << 20);
  hw_free(a);
  hw_stats s1{};
  hw_get_stats(&s1);
  void* b = hw_malloc(1 << 20);
  hw_stats s2{};
  hw_get_stats(&s2);
  EXPECT_EQ(s1.os_maps, s2.os_maps) << "second huge allocation should come from the cache";
  hw_free(b);
}

}  // namespace
