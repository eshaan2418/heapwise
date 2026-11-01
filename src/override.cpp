// Drop-in malloc replacement for Linux (LD_PRELOAD=libheapwise_override.so).
// macOS routes many allocations through malloc zones, which this shim does not replace;
// use the hw_* API there instead.
#include <unistd.h>

#include <cerrno>
#include <cstddef>

#include "heapwise/heapwise.h"

#if defined(__linux__)
extern "C" {
void* malloc(size_t size) { return hw_malloc(size); }
void free(void* ptr) { hw_free(ptr); }
void* calloc(size_t count, size_t size) { return hw_calloc(count, size); }
void* realloc(void* ptr, size_t size) { return hw_realloc(ptr, size); }
void* aligned_alloc(size_t alignment, size_t size) { return hw_aligned_alloc(alignment, size); }
void* memalign(size_t alignment, size_t size) { return hw_aligned_alloc(alignment, size); }
int posix_memalign(void** out, size_t alignment, size_t size) { return hw_posix_memalign(out, alignment, size); }
void* valloc(size_t size) { return hw_aligned_alloc(static_cast<size_t>(sysconf(_SC_PAGESIZE)), size); }
void* pvalloc(size_t size) {
  const auto page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  return hw_aligned_alloc(page, (size + page - 1) & ~(page - 1));
}
size_t malloc_usable_size(void* ptr) { return hw_owns(ptr) != 0 ? hw_usable_size(ptr) : 0; }
}
#endif
