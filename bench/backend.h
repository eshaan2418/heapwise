// Allocator backends for benchmarks. Each benchmark source is compiled once per backend
// into a separate executable, because jemalloc and mimalloc's static library both export
// unprefixed malloc and would otherwise replace the system allocator process-wide.
#pragma once
#include <cstddef>
#include <cstdlib>

#if defined(HW_BACKEND_HEAPWISE)
#include "heapwise/heapwise.h"
namespace backend {
inline const char* Name() { return "heapwise"; }
inline void* Malloc(std::size_t n) { return hw_malloc(n); }
inline void* Calloc(std::size_t n) { return hw_calloc(1, n); }
inline void* Realloc(void* p, std::size_t n) { return hw_realloc(p, n); }
inline void* Aligned(std::size_t a, std::size_t n) { return hw_aligned_alloc(a, n); }
inline void Free(void* p) { hw_free(p); }
inline const char* Version() { return "0.1.0"; }
}  // namespace backend
#elif defined(HW_BACKEND_MIMALLOC)
#include <mimalloc.h>

#include <cstdio>
namespace backend {
inline const char* Name() { return "mimalloc"; }
inline void* Malloc(std::size_t n) { return mi_malloc(n); }
inline void* Calloc(std::size_t n) { return mi_calloc(1, n); }
inline void* Realloc(void* p, std::size_t n) { return mi_realloc(p, n); }
inline void* Aligned(std::size_t a, std::size_t n) { return mi_malloc_aligned(n, a); }
inline void Free(void* p) { mi_free(p); }
inline const char* Version() {
  static char buf[16];
  const int v = mi_version();
  std::snprintf(buf, sizeof(buf), "%d.%d.%d", v / 100, (v / 10) % 10, v % 10);
  return buf;
}
}  // namespace backend
#else  // system or jemalloc: plain libc calls, resolved by the linker
#include <cstdio>
#if defined(HW_BACKEND_JEMALLOC)
extern "C" int mallctl(const char*, void*, std::size_t*, void*, std::size_t);
#endif
namespace backend {
#if defined(HW_BACKEND_JEMALLOC)
inline const char* Name() { return "jemalloc"; }
inline const char* Version() {
  const char* v = nullptr;
  std::size_t len = sizeof(v);
  if (mallctl("version", static_cast<void*>(&v), &len, nullptr, 0) != 0 || v == nullptr) std::abort();
  return v;
}
#else
inline const char* Name() { return "system"; }
inline const char* Version() { return "libc"; }
#endif
inline void* Malloc(std::size_t n) { return std::malloc(n); }
inline void* Calloc(std::size_t n) { return std::calloc(1, n); }
inline void* Realloc(void* p, std::size_t n) { return std::realloc(p, n); }
inline void* Aligned(std::size_t a, std::size_t n) {
  void* p = nullptr;
  return posix_memalign(&p, a < sizeof(void*) ? sizeof(void*) : a, n) == 0 ? p : nullptr;
}
inline void Free(void* p) { std::free(p); }
}  // namespace backend
#endif
