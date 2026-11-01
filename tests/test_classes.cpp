#include <gtest/gtest.h>

#include "internal.h"

namespace {

TEST(SizeClasses, RoundTripAndMonotonic) {
  for (std::uint32_t c = 0; c < hw::kNumClasses; ++c) {
    EXPECT_EQ(hw::SizeClass(hw::ClassSize(c)), c) << "class " << c;
    if (c > 0) EXPECT_GT(hw::ClassSize(c), hw::ClassSize(c - 1));
    EXPECT_EQ(hw::ClassSize(c) % hw::kMinAlign, 0u);
  }
}

TEST(SizeClasses, EverySizeFitsWithBoundedWaste) {
  for (std::size_t n = 0; n <= hw::kMaxSmallSize; ++n) {
    const std::uint32_t c = hw::SizeClass(n);
    ASSERT_LT(c, hw::kNumClasses) << n;
    const std::size_t cs = hw::ClassSize(c);
    ASSERT_GE(cs, n) << n;
    if (c > 0) ASSERT_LT(hw::ClassSize(c - 1), n == 0 ? 1 : n) << "not the smallest fitting class for " << n;
    if (n > 256) ASSERT_LE(cs - n, n / 4) << "internal fragmentation above 25% for " << n;
  }
}

TEST(SizeClasses, PagesHoldAtLeastEightBlocks) {
  for (std::uint32_t c = 0; c < hw::kNumClasses; ++c) {
    const std::size_t span = std::size_t{hw::ClassUnits(c)} * hw::kUnitSize;
    EXPECT_GE(span / hw::ClassSize(c), 8u) << "class " << c;
    EXPECT_LE(span / hw::ClassSize(c), 65535u);
  }
}

TEST(Layout, SegmentHeaderFitsInFirstUnit) {
  EXPECT_LE(sizeof(hw::Segment), hw::kUnitSize);
  EXPECT_EQ(alignof(hw::RemoteSlot), hw::kCacheLine);
}

}  // namespace
