#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "heapwise/heapwise.h"

namespace {

struct Msg {
  unsigned char* p;
  std::size_t n;
  unsigned char tag;
};

void Fill(const Msg& m) {
  for (std::size_t i = 0; i < m.n; ++i) m.p[i] = static_cast<unsigned char>(m.tag ^ i);
}
bool Ok(const Msg& m) {
  for (std::size_t i = 0; i < m.n; ++i) {
    if (m.p[i] != static_cast<unsigned char>(m.tag ^ i)) return false;
  }
  return true;
}

// Bounded MPMC queue used only by the tests.
class Queue {
 public:
  void Push(Msg m) {
    std::unique_lock<std::mutex> lk(mu_);
    not_full_.wait(lk, [&] { return q_.size() < 1024; });
    q_.push_back(m);
    not_empty_.notify_one();
  }
  bool Pop(Msg* out) {
    std::unique_lock<std::mutex> lk(mu_);
    not_empty_.wait(lk, [&] { return !q_.empty() || closed_; });
    if (q_.empty()) return false;
    *out = q_.front();
    q_.pop_front();
    not_full_.notify_one();
    return true;
  }
  void Close() {
    std::lock_guard<std::mutex> lk(mu_);
    closed_ = true;
    not_empty_.notify_all();
  }

 private:
  std::mutex mu_;
  std::condition_variable not_full_, not_empty_;
  std::deque<Msg> q_;
  bool closed_ = false;
};

TEST(Threads, ProducerConsumerCrossThreadFree) {
  constexpr int kProducers = 4, kConsumers = 4, kPerProducer = 40000;
  Queue q;
  std::atomic<int> bad{0}, freed{0};
  std::vector<std::thread> consumers;
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&] {
      Msg m{};
      while (q.Pop(&m)) {
        if (!Ok(m)) bad.fetch_add(1);
        hw_free(m.p);
        freed.fetch_add(1);
      }
    });
  }
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      std::mt19937 rng(static_cast<unsigned>(p));
      for (int i = 0; i < kPerProducer; ++i) {
        const std::size_t n = 1 + rng() % (i % 50 == 0 ? 70000 : 512);
        Msg m{static_cast<unsigned char*>(hw_malloc(n)), n, static_cast<unsigned char>(rng())};
        Fill(m);
        q.Push(m);
        if (i % 1000 == 0) hw_collect(0);  // producers reclaim remote frees
      }
    });
  }
  for (auto& t : producers) t.join();
  q.Close();
  for (auto& t : consumers) t.join();
  EXPECT_EQ(bad.load(), 0);
  EXPECT_EQ(freed.load(), kProducers * kPerProducer);
}

TEST(Threads, ExitedThreadsSegmentsAreAdoptedAndReclaimed) {
  hw_collect(1);
  std::vector<Msg> leftovers;
  std::mutex mu;
  for (int round = 0; round < 4; ++round) {
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
      ts.emplace_back([&, t] {
        std::vector<Msg> mine;
        for (int i = 0; i < 20000; ++i) {
          const std::size_t n = 16 + static_cast<std::size_t>((i * 37 + t) % 2000);
          Msg m{static_cast<unsigned char*>(hw_malloc(n)), n, static_cast<unsigned char>(i)};
          Fill(m);
          mine.push_back(m);
        }
        // Free every other block locally; hand the rest to the main thread.
        std::lock_guard<std::mutex> lk(mu);
        for (std::size_t i = 0; i < mine.size(); ++i) {
          if (i % 2 == 0) {
            hw_free(mine[i].p);
          } else {
            leftovers.push_back(mine[i]);
          }
        }
      });
    }
    for (auto& t : ts) t.join();
  }
  hw_stats mid{};
  hw_get_stats(&mid);
  EXPECT_GT(mid.segments_abandoned, 0u) << "exited threads left live blocks behind";

  // Free from the main thread: these are remote frees into abandoned segments.
  for (const Msg& m : leftovers) {
    ASSERT_TRUE(Ok(m));
    hw_free(m.p);
  }
  // A new thread adopts abandoned segments when it needs memory.
  std::thread([] {
    std::vector<void*> v;
    for (int i = 0; i < 1000; ++i) v.push_back(hw_malloc(64));
    for (void* p : v) hw_free(p);
  }).join();
  hw_collect(1);
  hw_stats after{};
  hw_get_stats(&after);
  EXPECT_EQ(after.segments_abandoned, 0u);
  EXPECT_LE(after.segments_mapped, 2u);
}

TEST(Threads, LarsonStyleExchange) {
  constexpr int kThreads = 8, kSlots = 2048, kIters = 60000;
  struct Slot {
    std::atomic<unsigned char*> p{nullptr};
    std::atomic<std::size_t> n{0};
  };
  std::vector<Slot> slots(kSlots);
  std::atomic<int> bad{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      std::mt19937 rng(static_cast<unsigned>(t * 7919));
      for (int i = 0; i < kIters; ++i) {
        const std::size_t n = 16 + rng() % 1000;
        auto* p = static_cast<unsigned char*>(hw_malloc(n));
        std::memset(p, static_cast<int>(n & 0xff), n);
        // Store size in the first bytes so the freeing thread can verify the block.
        std::memcpy(p, &n, sizeof(n) < n ? sizeof(n) : n);
        Slot& s = slots[rng() % kSlots];
        unsigned char* old = s.p.exchange(p, std::memory_order_acq_rel);
        if (old != nullptr) {
          std::size_t on = 0;
          std::memcpy(&on, old, sizeof(on));
          if (on < 16 || on > 1016 || old[on - 1] != static_cast<unsigned char>(on & 0xff)) bad.fetch_add(1);
          hw_free(old);
        }
      }
    });
  }
  for (auto& t : ts) t.join();
  for (auto& s : slots) hw_free(s.p.load());
  EXPECT_EQ(bad.load(), 0);
}

}  // namespace

namespace {

// Regression: remote frees into full pages must be reclaimed promptly. An earlier rescan
// heuristic let a producer's heap double every cycle (2.4 GB for ~1 MB of live data).
TEST(Threads, ProducerHeapStaysBoundedUnderRemoteFrees) {
  hw_collect(1);
  constexpr std::size_t kCap = 4096;
  std::vector<std::atomic<void*>> ring(kCap);
  std::atomic<std::size_t> head{0}, tail{0};
  constexpr std::size_t kItems = 3000000;
  std::atomic<std::uint64_t> peak{0};
  std::thread consumer([&] {
    for (std::size_t i = 0; i < kItems; ++i) {
      while (head.load(std::memory_order_acquire) == i) std::this_thread::yield();
      hw_free(ring[i % kCap].load(std::memory_order_relaxed));
      tail.store(i + 1, std::memory_order_release);
    }
  });
  std::thread producer([&] {
    for (std::size_t i = 0; i < kItems; ++i) {
      void* m = hw_malloc(32 + (i * 2654435761u) % 480);
      while (i - tail.load(std::memory_order_acquire) == kCap) std::this_thread::yield();
      ring[i % kCap].store(m, std::memory_order_relaxed);
      head.store(i + 1, std::memory_order_release);
      if (i % 65536 == 0) {
        hw_stats st{};
        hw_get_stats(&st);
        if (st.bytes_mapped > peak.load()) peak.store(st.bytes_mapped);
      }
    }
  });
  producer.join();
  consumer.join();
  EXPECT_LT(peak.load(), std::uint64_t{64} << 20) << "live data is ~1 MB; mapped memory should stay small";
}

// A thread that frees into a segment abandoned by an exited thread adopts it, so the memory
// is released as soon as the last block is freed, without an explicit hw_collect(1).
TEST(Threads, FreeIntoAbandonedSegmentAdoptsIt) {
  hw_collect(1);
  std::vector<void*> ptrs;
  std::thread([&] {
    for (int i = 0; i < 100000; ++i) ptrs.push_back(hw_malloc(128));
  }).join();
  hw_stats mid{};
  hw_get_stats(&mid);
  EXPECT_GT(mid.segments_abandoned, 0u);
  for (void* p : ptrs) hw_free(p);
  hw_stats after{};
  hw_get_stats(&after);
  EXPECT_EQ(after.segments_abandoned, 0u);
  EXPECT_LT(after.bytes_mapped, mid.bytes_mapped);
}

}  // namespace

#include <sys/wait.h>
#include <unistd.h>

namespace {

// fork() while other threads hammer the allocator: the child must be able to allocate
// (atfork handlers keep the global locks consistent).
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define HW_TSAN 1
#endif
#endif

TEST(Threads, ForkWhileOtherThreadsAllocate) {
#if defined(HW_TSAN)
  GTEST_SKIP() << "ThreadSanitizer does not support threads in a child of a multithreaded fork";
#endif
  std::atomic<bool> stop{false};
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; ++t) {
    ts.emplace_back([&] {
      while (!stop.load()) {
        std::vector<void*> v;
        for (int i = 0; i < 64; ++i) v.push_back(hw_malloc(i % 2 == 0 ? 64 : 400000));
        for (void* p : v) hw_free(p);
        std::thread([] { hw_free(hw_malloc(32)); }).join();  // heap pool + abandon paths
      }
    });
  }
  for (int i = 0; i < 20; ++i) {
    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      std::thread child([] {
        for (int k = 0; k < 1000; ++k) hw_free(hw_malloc(static_cast<std::size_t>(k) * 997 % 500000));
      });
      child.join();
      _exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  stop.store(true);
  for (auto& t : ts) t.join();
}

}  // namespace
