// macOS allocation tracer. Load with
//   HWTRACE_DIR=/tmp/out DYLD_INSERT_LIBRARIES=libhwtracer.dylib <program>
// Every malloc-family call made by the program is appended to <dir>/trace.<pid>.raw as
// fixed 32-byte records. Calls made by this dylib itself are not interposed, so writing
// the trace cannot recurse. SIP-protected binaries (/usr/bin, /System) ignore DYLD_* vars.
#include <fcntl.h>
#include <malloc/malloc.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define INTERPOSE(repl, orig)                                                          \
  __attribute__((used)) static const struct {                                          \
    const void* replacement;                                                           \
    const void* replacee;                                                              \
  } interpose_##orig __attribute__((section("__DATA,__interpose"))) = {(const void*)&repl, \
                                                                      (const void*)&orig}

typedef struct {
  uint8_t op;  // 'M' malloc, 'C' calloc, 'R' realloc, 'F' free, 'A' aligned
  uint8_t pad[3];
  uint32_t tid;
  uint64_t a, b, c;
} Rec;
_Static_assert(sizeof(Rec) == 32, "record layout");

enum { kBuf = 16384 };
static Rec g_buf[kBuf];
static int g_n;
static int g_fd = -1;
static int g_failed;
static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT;

static void open_out(void) {
  const char* dir = getenv("HWTRACE_DIR");
  if (dir == NULL) {
    g_failed = 1;
    return;
  }
  char path[1024];
  size_t len = strlen(dir);
  if (len > 900) {
    g_failed = 1;
    return;
  }
  memcpy(path, dir, len);
  const char* mid = "/trace.";
  memcpy(path + len, mid, 7);
  len += 7;
  char digits[24];
  int nd = 0;
  unsigned long pid = (unsigned long)getpid();
  do {
    digits[nd++] = (char)('0' + pid % 10);
    pid /= 10;
  } while (pid != 0);
  while (nd > 0) path[len++] = digits[--nd];
  memcpy(path + len, ".raw", 5);
  g_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (g_fd < 0) g_failed = 1;
}

static void flush_locked(void) {
  if (g_n == 0 || g_fd < 0) return;
  const char* p = (const char*)g_buf;
  size_t left = (size_t)g_n * sizeof(Rec);
  while (left > 0) {
    ssize_t w = write(g_fd, p, left);
    if (w <= 0) break;
    p += w;
    left -= (size_t)w;
  }
  g_n = 0;
}

static void emit(uint8_t op, uint64_t a, uint64_t b, uint64_t c) {
  os_unfair_lock_lock(&g_lock);
  if (g_fd < 0 && !g_failed) open_out();
  if (!g_failed) {
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    Rec* r = &g_buf[g_n++];
    r->op = op;
    r->tid = (uint32_t)tid;
    r->a = a;
    r->b = b;
    r->c = c;
    if (g_n == kBuf) flush_locked();
  }
  os_unfair_lock_unlock(&g_lock);
}

__attribute__((destructor)) static void finish(void) {
  os_unfair_lock_lock(&g_lock);
  flush_locked();
  os_unfair_lock_unlock(&g_lock);
}

#define U(x) ((uint64_t)(uintptr_t)(x))

static void* t_malloc(size_t n) {
  void* p = malloc(n);
  emit('M', n, 0, U(p));
  return p;
}
static void* t_calloc(size_t k, size_t n) {
  void* p = calloc(k, n);
  emit('C', (uint64_t)k * n, 0, U(p));
  return p;
}
static void* t_realloc(void* old, size_t n) {
  void* p = realloc(old, n);
  emit('R', U(old), n, U(p));
  return p;
}
static void t_free(void* p) {
  if (p != NULL) emit('F', U(p), 0, 0);
  free(p);
}
static int t_posix_memalign(void** out, size_t align, size_t n) {
  int rc = posix_memalign(out, align, n);
  if (rc == 0) emit('A', align, n, U(*out));
  return rc;
}
static void* t_aligned_alloc(size_t align, size_t n) {
  void* p = aligned_alloc(align, n);
  emit('A', align, n, U(p));
  return p;
}
static void* t_valloc(size_t n) {
  void* p = valloc(n);
  emit('A', 16384, n, U(p));
  return p;
}
static void* t_zone_malloc(malloc_zone_t* z, size_t n) {
  void* p = malloc_zone_malloc(z, n);
  emit('M', n, 0, U(p));
  return p;
}
static void* t_zone_calloc(malloc_zone_t* z, size_t k, size_t n) {
  void* p = malloc_zone_calloc(z, k, n);
  emit('C', (uint64_t)k * n, 0, U(p));
  return p;
}
static void* t_zone_realloc(malloc_zone_t* z, void* old, size_t n) {
  void* p = malloc_zone_realloc(z, old, n);
  emit('R', U(old), n, U(p));
  return p;
}
static void t_zone_free(malloc_zone_t* z, void* p) {
  if (p != NULL) emit('F', U(p), 0, 0);
  malloc_zone_free(z, p);
}
static void* t_zone_memalign(malloc_zone_t* z, size_t align, size_t n) {
  void* p = malloc_zone_memalign(z, align, n);
  emit('A', align, n, U(p));
  return p;
}

INTERPOSE(t_malloc, malloc);
INTERPOSE(t_calloc, calloc);
INTERPOSE(t_realloc, realloc);
INTERPOSE(t_free, free);
INTERPOSE(t_posix_memalign, posix_memalign);
INTERPOSE(t_aligned_alloc, aligned_alloc);
INTERPOSE(t_valloc, valloc);
INTERPOSE(t_zone_malloc, malloc_zone_malloc);
INTERPOSE(t_zone_calloc, malloc_zone_calloc);
INTERPOSE(t_zone_realloc, malloc_zone_realloc);
INTERPOSE(t_zone_free, malloc_zone_free);
INTERPOSE(t_zone_memalign, malloc_zone_memalign);
