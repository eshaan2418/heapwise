// Model-based randomized test: random malloc/calloc/realloc/aligned/free sequences checked
// against a reference model for content integrity, alignment, and non-overlap.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <vector>

#include "heapwise/heapwise.h"

namespace {

struct Live {
  unsigned char* p;
  std::size_t n;
  unsigned char tag;
};

class Model {
 public:
  void Add(const Live& l) {
    const auto lo = reinterpret_cast<std::uintptr_t>(l.p);
    const std::uintptr_t hi = lo + hw_usable_size(l.p);
    auto it = ranges_.upper_bound(lo);
    if (it != ranges_.end()) ASSERT_GE(it->first, hi) << "overlaps next block";
    if (it != ranges_.begin()) ASSERT_LE(std::prev(it)->second, lo) << "overlaps previous block";
    ranges_[lo] = hi;
  }
  void Remove(const void* p) { ranges_.erase(reinterpret_cast<std::uintptr_t>(p)); }

 private:
  std::map<std::uintptr_t, std::uintptr_t> ranges_;
};

void Fill(const Live& l) {
  for (std::size_t i = 0; i < l.n; ++i) l.p[i] = static_cast<unsigned char>(l.tag + i);
}

void Check(const Live& l, std::size_t upto) {
  for (std::size_t i = 0; i < upto; ++i) {
    ASSERT_EQ(l.p[i], static_cast<unsigned char>(l.tag + i)) << "corruption at byte " << i << " of " << l.n;
  }
}

std::size_t RandomSize(std::mt19937_64& rng) {
  const auto r = rng() % 100;
  if (r < 70) return rng() % 256;
  if (r < 95) return rng() % 16384;
  if (r < 99) return rng() % 300000;
  return rng() % (4u << 20);
}

void RunSeed(std::uint64_t seed, int ops) {
  std::mt19937_64 rng(seed);
  std::vector<Live> live;
  Model model;
  for (int op = 0; op < ops; ++op) {
    const auto r = rng() % 100;
    if (r < 45 || live.empty()) {
      const std::size_t n = RandomSize(rng);
      Live l{nullptr, n, static_cast<unsigned char>(rng())};
      const auto kind = rng() % 10;
      if (kind == 0) {
        l.p = static_cast<unsigned char*>(hw_calloc(1, n));
        for (std::size_t i = 0; i < n; ++i) ASSERT_EQ(l.p[i], 0);
      } else if (kind == 1) {
        const std::size_t a = std::size_t{1} << (rng() % 18);
        l.p = static_cast<unsigned char*>(hw_aligned_alloc(a, n));
        ASSERT_EQ(reinterpret_cast<std::uintptr_t>(l.p) % a, 0u);
      } else {
        l.p = static_cast<unsigned char*>(hw_malloc(n));
      }
      ASSERT_NE(l.p, nullptr);
      ASSERT_GE(hw_usable_size(l.p), n);
      ASSERT_NO_FATAL_FAILURE(model.Add(l));
      Fill(l);
      live.push_back(l);
    } else if (r < 60) {
      Live& l = live[rng() % live.size()];
      const std::size_t n = RandomSize(rng);
      model.Remove(l.p);
      auto* q = static_cast<unsigned char*>(hw_realloc(l.p, n));
      ASSERT_NE(q, nullptr);
      Live nl{q, n, l.tag};
      ASSERT_NO_FATAL_FAILURE(Check(nl, l.n < n ? l.n : n));
      ASSERT_NO_FATAL_FAILURE(model.Add(nl));
      Fill(nl);
      l = nl;
    } else {
      const std::size_t i = rng() % live.size();
      ASSERT_NO_FATAL_FAILURE(Check(live[i], live[i].n));
      model.Remove(live[i].p);
      hw_free(live[i].p);
      live[i] = live.back();
      live.pop_back();
    }
  }
  for (const Live& l : live) {
    ASSERT_NO_FATAL_FAILURE(Check(l, l.n));
    hw_free(l.p);
  }
}

class RandomOps : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(RandomOps, MatchesModel) { RunSeed(GetParam(), 20000); }

INSTANTIATE_TEST_SUITE_P(Seeds, RandomOps, ::testing::Values(1, 2, 3, 42, 1234, 99991));

}  // namespace
