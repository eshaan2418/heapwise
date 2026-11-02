// Compact replay format (.hwt): object ids instead of addresses.
#pragma once
#include <cstdint>

namespace trace {

constexpr char kMagic[4] = {'H', 'W', 'T', '1'};

enum Kind : std::uint8_t { kAlloc = 0, kFree = 1, kRealloc = 2, kAligned = 3, kCalloc = 4 };

struct Header {
  char magic[4];
  std::uint32_t version;
  std::uint64_t ops;
  std::uint64_t ids;
};

struct Op {
  std::uint8_t kind;
  std::uint8_t align_log2;
  std::uint16_t reserved;
  std::uint32_t id;
  std::uint64_t size;
};
static_assert(sizeof(Op) == 16);

// Raw record written by tools/tracer.
struct RawRec {
  std::uint8_t op;
  std::uint8_t pad[3];
  std::uint32_t tid;
  std::uint64_t a, b, c;
};
static_assert(sizeof(RawRec) == 32);

}  // namespace trace
