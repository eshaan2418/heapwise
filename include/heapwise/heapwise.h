// heapwise: a general-purpose, thread-caching memory allocator.
//
// All functions are thread-safe. Memory returned by hw_* must be released with
// hw_free (or resized with hw_realloc); passing pointers from another allocator
// aborts with a diagnostic.
#ifndef HEAPWISE_HEAPWISE_H
#define HEAPWISE_HEAPWISE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Allocate at least `size` bytes aligned to 16. malloc(0) returns a unique,
// freeable pointer. Returns NULL and sets errno=ENOMEM on failure.
void* hw_malloc(size_t size);

// Allocate zeroed memory for `count * size` bytes; NULL on overflow.
void* hw_calloc(size_t count, size_t size);

// Resize; may move. hw_realloc(NULL, n) == hw_malloc(n).
// hw_realloc(p, 0) returns a minimum-size block (macOS semantics), never frees.
void* hw_realloc(void* ptr, size_t size);

// Release memory. hw_free(NULL) is a no-op.
void hw_free(void* ptr);

// Aligned allocation. `alignment` must be a power of two <= 2 MiB.
void* hw_aligned_alloc(size_t alignment, size_t size);
int hw_posix_memalign(void** out, size_t alignment, size_t size);

// Number of usable bytes in the block (>= the requested size).
size_t hw_usable_size(const void* ptr);

// True if `ptr` points into memory managed by heapwise.
int hw_owns(const void* ptr);

// Reclaim memory: collects pending cross-thread frees for the calling thread,
// returns empty pages and cached segments to the OS. With `force`, also adopts
// segments abandoned by exited threads so their free memory can be reclaimed.
void hw_collect(int force);

typedef struct hw_stats {
  uint64_t segments_mapped;     // live 4 MiB segments
  uint64_t huge_mapped;         // live dedicated huge mappings
  uint64_t bytes_mapped;        // total bytes currently mapped from the OS
  uint64_t segments_abandoned;  // segments waiting to be adopted
  uint64_t os_maps;             // cumulative mmap calls
  uint64_t os_unmaps;           // cumulative munmap calls
} hw_stats;

void hw_get_stats(hw_stats* out);

#ifdef __cplusplus
}
#endif

#endif  // HEAPWISE_HEAPWISE_H
