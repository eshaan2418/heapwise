# heapwise deep dive

Hard questions about the design, with answers grounded in the code. Function names refer to
`src/heapwise.cpp` unless noted.

---

### 1. How does `free` find an object's metadata without a header in front of every block?

Every small block lives inside a 4 MiB segment aligned to 4 MiB, so `ptr & ~kSegmentMask`
(`SegmentOf`) is the segment header. `(ptr - segment) >> 16` is the 64 KiB unit index
(`UnitOf`), and `pages[unit].head` points at the span's head metadata. That is two dependent
loads after the mask, with no hash table and no per-block header. Per-block headers would
cost 8–16 bytes per allocation, which is 50–100% overhead for 16-byte objects. They would
also put metadata next to user data, where buffer overruns corrupt it.

Huge objects use the same trick: their mapping is also 4 MiB-aligned with a `Segment`
header at its base, and the object starts at most 2 MiB + 64 KiB into it. Masking any valid
huge pointer therefore lands on its header too.

### 2. Why 4 MiB segments and 64 KiB units?

- **4 MiB** keeps the one-bit-per-segment ownership map small: 2^26 bits = 8 MiB of virtual
  memory for a 48-bit address space, committed only where bits are set. It is also a multiple
  of the 2 MiB huge page, so `HEAPWISE_HUGEPAGES=1` on Linux covers each segment with two huge
  pages. And it is large enough that `mmap` is rare: one call per 4 MiB of heap growth.
- **64 KiB** units give the free-unit bitmap exactly 64 bits (one `uint64_t`, with span search
  done by shifting and ANDing, see `SegmentAllocSpan`). A unit holds 4,096 of the smallest
  blocks, so `used`/`capacity` fit in `uint16_t`, and the 12 KiB header fits in unit 0.

The cost is up to one partly used segment per thread per size-class mix. With 64 threads
that is at least 256 MiB of address space, though only touched pages count against RSS.

### 3. The local free path has no atomics. What makes that safe?

Ownership. Only the heap stored in `Segment::owner` touches a page's non-atomic fields.
`hw_free` compares `owner` with the calling thread's heap. On a match it is the only thread
that can be touching those fields, so plain loads and stores are enough. On a mismatch it
touches only the atomic `RemoteSlot::head`.

The `owner` load is `relaxed`. It can only equal my heap if my thread stored it (when the
segment was created or adopted), and a thread always observes its own stores. Another thread
may read a stale value, but a stale value is never *its own* heap (see question 9), so the
worst case is a correct remote push.

### 4. Why a Treiber stack for remote frees, and where is the ABA problem?

There isn't one, because nobody pops single nodes. Pushers CAS a block onto `RemoteSlot::head`.
The owner takes the whole list with `exchange(nullptr)` in `PageCollect`. ABA needs a
pop that reads `head`, reads `head->next`, and CASes `head` to `next` while the node is
recycled underneath it. A take-all exchange never reads `next` before it owns the list.

Ordering: the push CAS is `release`, publishing the `next` pointer written into the block.
The exchange is `acquire`, so the owner sees those `next` pointers when it walks the list.
The initial relaxed `load` in `PageCollect` is only a hint to skip the exchange when the
list is empty.

### 5. When can a page's memory be handed back to its segment, and why can't that race with a remote free?

Only when `used == 0` after collecting remote frees (`FreeLocalSlow`, `ScanFull`, `Adopt`,
`hw_collect`). `used` counts blocks handed out and not yet returned to the owner's free list.
A block being freed remotely is still counted until the owner collects it. So while any
remote push is in flight, or sitting uncollected in the stack, `used > 0` and the span stays.
The same argument makes `TryAdoptOnFree` safe: the freeing thread holds a live block, so
the segment cannot be unmapped while it waits on the abandoned-list mutex.

### 6. Walk through the bug that made a producer's heap grow to 2.4 GB.

Remote frees into *full* pages are invisible until the owner scans the full list. The first
heuristic scanned after allocating as many new pages as were on the full list, then reset the
counter. In a producer/consumer run the scan does reclaim almost everything. But those pages
go back into the queue, fill up, and return to the full list, so the next scan again waits
for a full-list's worth of new pages. Each cycle doubled the heap. The pilot run recorded
2.4 GB peak RSS for about 1 MB of live data (`bench/results/history/`).

The fix (`scan_budget`): the next scan waits for as many new pages as the last scan *failed*
to reclaim. If the scan reclaimed everything, the budget is ~0 and the next scan happens as
soon as the queue runs dry. If it reclaimed nothing (a growing heap), the budget equals the
full-list length, so the O(n) scan is paid for by n page allocations: amortised O(1). The
regression test bounds mapped memory at 64 MiB for 3M messages.

The alternative is mimalloc's approach: on a remote free into a full page, the freeing thread
notifies the owning heap through a delayed-free list. That reacts faster but adds a second
atomic protocol, with page and heap state machines, to the remote free path. The budget
heuristic needs no extra state on the remote path.

### 7. Why does a full page wait for 1/8 of its blocks to be free before rejoining the queue?

Without hysteresis, one local free into a full page moves it to the queue head, and the next
malloc takes that one block. On the malloc after that, the slow path finds the page empty
again and moves it back. In random-replacement workloads (Larson) most pages hover at full,
so nearly every malloc/free pair paid two list moves and a slow-path call. Single-threaded
Larson went from about 80 ms to about 62 ms with the threshold.

The cost: up to 1/8 of a full page's blocks can sit unused until more frees arrive or the
page is collected by `hw_collect`. That bounds the extra memory at 12.5% of pages that are
otherwise full.

### 8. What happens to a thread's memory when it exits?

`ThreadExit`, registered as the pthread key destructor, collects remote frees for every page,
releases empty spans, unmaps fully empty segments, and puts the rest on the global abandoned
list with `owner = nullptr`. Frees into those segments from any thread take the remote path.
A segment is taken over when:

- a heap needs a new segment (`SegmentAcquire` prefers abandoned over `mmap`);
- a thread frees into it (`TryAdoptOnFree`), which turns the remaining frees local and lets
  empty pages be released immediately;
- someone calls `hw_collect(1)`.

Without adopt-on-free, single-threaded Larson (one new thread per round) sent every free in
rounds 2–8 down the atomic remote path into segments no one owned.

The remaining gap: abandoned segments that nobody frees into and nobody needs keep their
pages until `hw_collect(1)`. A background reclaimer would fix this at the cost of a thread.

### 9. Heaps are recycled. Could a new thread see `owner == its heap` on a segment it never adopted?

No. The dying thread stores `owner = nullptr` on every segment it abandons, then returns its
`Heap` to the pool under `g_heap_mu`. The new thread gets the recycled `Heap*` from the same
mutex, so its acquire synchronises with the dying thread's release, and the null stores
happen-before anything the new thread reads. For the new heap to appear in a segment's
`owner`, the new thread itself must store it. Without the mutex this would be a real ABA bug:
a stale owner pointer equal to the new heap would send another thread's page into the local
path.

### 10. How does `hw_aligned_alloc` work without a separate aligned allocator?

Spans start on 64 KiB boundaries and blocks sit at `start + i * block_size`. If the block
size is a multiple of the alignment, every block is aligned. Every power of two from 16 B to
256 KiB is a size class, so `hw_aligned_alloc` rounds the request up to at least the
alignment and walks up to the first class whose size divides evenly. For alignments up to
64 KiB that always succeeds below the 256 KiB limit. Larger alignments, or larger sizes, use
a huge mapping with the object offset set to the alignment (the base is 4 MiB-aligned), up
to 2 MiB. The cost is waste: `aligned_alloc(4096, 100)` uses a 4 KiB block. Allocators that
round to the alignment inside a larger block pay the same.

### 11. Why carve blocks lazily (`PageExtend`) instead of threading the whole page into a free list?

Threading a fresh 64 KiB page of 16-byte blocks writes 4,096 `next` pointers, which touches
(commits) the whole page even if the program uses three blocks. `PageExtend` carves about
4 KiB per call, so RSS tracks what the program actually uses. It also keeps new blocks in
address order, which helps the hardware prefetcher. The cost is an occasional slow-path call
per ~4 KiB of new allocations.

### 12. On macOS, why read `TPIDRRO_EL0` instead of using `thread_local`?

On macOS, every `thread_local` access in compiled code goes through a TLV descriptor call
(`_tlv_get_addr`). The profile showed it at about 5% of single-threaded Larson. pthread keys
on Apple arm64 live in an array whose base is in `TPIDRRO_EL0`; Apple's inline
`_pthread_getspecific_direct` reads it this way. `CurrentHeap` reads our key's slot directly:
one `mrs` and one load.

The risk is that this layout is not a documented ABI. `GlobalInit` therefore stores a probe
value with `pthread_setspecific` and checks it can be read back through the direct path.
Only then does it publish the slot index in `g_tsd_slot` (a single atomic word, so readers
never see a half-initialised key). If the check ever fails, every lookup falls back to
`thread_local`. The measured gain was small (~2% on single-threaded Larson), and the design
keeps the fallback as the default on every other platform.

### 13. Why is there no `malloc` override on macOS?

On Linux, defining `malloc`/`free` in an `LD_PRELOAD` library replaces the allocator for the
whole process. On macOS, system frameworks call `malloc_zone_malloc(default_zone, …)` and
`malloc_zone_free` directly. Interposing `malloc` alone would let heapwise pointers reach the
system zone's `free` (a crash), or system-zone pointers reach `hw_free` (an abort).
mimalloc and jemalloc solve this by registering a full malloc zone, with introspection
callbacks, and making it the default. That is a large surface to test and is out of scope.
heapwise on macOS is used through the `hw_*` API.

### 14. What does the benchmark methodology get right, and what are its limits?

Right:

- One executable per allocator, because jemalloc and the mimalloc static library both export
  `malloc`. The jemalloc binary checks `mallctl("version")` at startup, which proves jemalloc
  is the allocator actually in use.
- Real traces from four real programs, recorded by a DYLD interposer with ids instead of
  addresses, so every allocator replays the identical sequence.
- 7 repetitions, interleaved round-robin across allocators, so drift hits all of them equally.
- `run.sh` waits for the 1-minute load average to fall below 3 and records the load next to
  every result. An early full run was discarded because another job was using the machine.

Limits:

- Multithreaded traces (ffmpeg) are replayed on one thread in recorded global order, so they
  measure single-thread behaviour on a real size and lifetime mix.
- Replay touches only the first word and one byte per 4 KiB of each object. Real programs
  touch more, so absolute times understate cache effects, equally for all allocators.
- Peak RSS is process-wide (`getrusage`) and includes the replay's own trace buffer, which is
  the same for all allocators.
- The producer/consumer workload spins with `yield`, so its timings depend on scheduler
  behaviour and have wide confidence intervals.

### 15. Where does heapwise lose, and why?

See the README table for numbers. On the replays and on Larson, mimalloc and jemalloc are
faster. Likely reasons, roughly in order:

1. mimalloc keeps a separate `local_free` list and swaps it in only when `free` runs dry.
   That batches slow-path work and makes it more predictable; heapwise reuses freed blocks
   immediately and calls the slow path more often.
2. jemalloc's tcache caches a stack of pointers per size class. It wins Larson because frees
   and allocations become array pushes and pops, with no page metadata touched at all.
3. heapwise checks the segment-map bit on every free (one extra cache line). That buys a clean
   abort on foreign pointers instead of silent corruption, and it is a deliberate choice.
4. Huge allocations (the ffmpeg trace has frames up to 11 MB) go through a mutex-protected
   cache and `madvise`, which costs more than jemalloc's extent reuse.

### 16. What would break first in production?

- **Double free**: not detected. A double local free corrupts the free list; a double remote
  free corrupts `used`. Detection costs a bit per block or free-list encoding; mimalloc's
  secure mode has both.
- **Huge-allocation contention**: one global mutex. Many threads allocating >256 KiB buffers
  at once will serialise on it.
- **Abandoned memory**: segments of exited threads that no one touches stay mapped until
  adoption or `hw_collect(1)`.
- **Purging policy**: freed spans inside a live segment are not purged until the whole
  segment empties, so a heap that shrinks from its peak keeps most of its RSS until then.

### 17. What happens on out-of-memory and on absurd sizes?

`mmap` failure propagates: the slow path returns `nullptr` and sets `errno = ENOMEM`.
`hw_calloc` checks `count * size` with `__builtin_mul_overflow`. `hw_malloc` refuses sizes
above `SIZE_MAX / 4` before computing mapping sizes, so the rounding arithmetic cannot
overflow. Bad alignments return `EINVAL`. All of these paths are tested in `test_basic.cpp`.

### 18. How is correctness tested?

- Exhaustive size-class checks for every size from 0 to 256 KiB: smallest fitting class,
  bounded waste, at least 8 blocks per page.
- A model-based random test (6 seeds × 20,000 operations) mixing malloc, calloc, realloc,
  aligned allocation, and free. It checks every byte of every live block with a per-block
  pattern, and checks that no two live blocks overlap, using an interval map over
  `hw_usable_size`.
- Thread tests: producer/consumer cross-thread frees with content checks, Larson-style
  exchange, thread churn with abandonment and adoption, the bounded-heap regression,
  adopt-on-free, and fork while other threads allocate.
- All of it under ASan+UBSan and TSan in CI, plus an `LD_PRELOAD` run of real programs on
  Linux (multithreaded Python, `sort`, `git`). That run first proves heapwise is active: a
  17-byte request reports a 32-byte usable size, where glibc reports 24.

### 19. If you had another week, what would you change?

1. Delayed-free notification for full pages, then measure it against the budget heuristic on
   producer/consumer.
2. A `local_free`/`free` split (question 15, item 1), measured on the four traces.
3. Purge free spans after a delay, so RSS falls when a heap shrinks.
4. Per-size-class huge caches without a global lock.
5. An optional hardened mode: encoded free-list pointers, guard pages between segments, and
   double-free detection with a per-page bitmap.
