// Converts raw tracer output (addresses) into the .hwt replay format (dense object ids)
// and prints a summary of the trace.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "trace_format.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <in.raw> <out.hwt>\n", argv[0]);
    return 2;
  }
  std::FILE* in = std::fopen(argv[1], "rb");
  if (in == nullptr) {
    std::perror(argv[1]);
    return 1;
  }
  std::vector<trace::Op> ops;
  std::unordered_map<std::uint64_t, std::uint32_t> live;
  std::vector<std::uint64_t> sizes;  // current size per id
  std::uint64_t live_bytes = 0, peak_bytes = 0, skipped_free = 0, implicit_free = 0, threads = 0;
  std::unordered_map<std::uint32_t, bool> tids;
  std::vector<std::uint64_t> alloc_sizes;
  std::uint32_t next_id = 0;

  auto release = [&](std::uint32_t id) {
    live_bytes -= sizes[id];
    sizes[id] = 0;
  };
  auto add = [&](std::uint64_t ptr, std::uint64_t size, std::uint8_t kind, std::uint8_t align_log2) {
    if (auto it = live.find(ptr); it != live.end()) {  // freed through a path we did not see
      ops.push_back({trace::kFree, 0, 0, it->second, 0});
      release(it->second);
      live.erase(it);
      ++implicit_free;
    }
    const std::uint32_t id = next_id++;
    sizes.push_back(size);
    live[ptr] = id;
    live_bytes += size;
    peak_bytes = std::max(peak_bytes, live_bytes);
    alloc_sizes.push_back(size);
    ops.push_back({kind, align_log2, 0, id, size});
  };

  trace::RawRec r{};
  while (std::fread(&r, sizeof(r), 1, in) == 1) {
    tids[r.tid] = true;
    switch (r.op) {
      case 'M':
      case 'C':
        if (r.c != 0) add(r.c, r.a, r.op == 'M' ? trace::kAlloc : trace::kCalloc, 0);
        break;
      case 'A':
        if (r.c != 0) add(r.c, r.b, trace::kAligned, static_cast<std::uint8_t>(__builtin_ctzll(r.a | 16)));
        break;
      case 'F':
        if (auto it = live.find(r.a); it != live.end()) {
          ops.push_back({trace::kFree, 0, 0, it->second, 0});
          release(it->second);
          live.erase(it);
        } else {
          ++skipped_free;
        }
        break;
      case 'R': {
        if (r.c == 0) break;
        auto it = r.a == 0 ? live.end() : live.find(r.a);
        if (it == live.end()) {
          add(r.c, r.b, trace::kAlloc, 0);
          break;
        }
        const std::uint32_t id = it->second;
        live.erase(it);
        live_bytes = live_bytes - sizes[id] + r.b;
        sizes[id] = r.b;
        peak_bytes = std::max(peak_bytes, live_bytes);
        if (auto clash = live.find(r.c); clash != live.end()) {
          ops.push_back({trace::kFree, 0, 0, clash->second, 0});
          release(clash->second);
          live.erase(clash);
          ++implicit_free;
        }
        live[r.c] = id;
        ops.push_back({trace::kRealloc, 0, 0, id, r.b});
        break;
      }
      default:
        std::fprintf(stderr, "bad record op %d\n", r.op);
        return 1;
    }
  }
  std::fclose(in);
  threads = tids.size();

  std::FILE* out = std::fopen(argv[2], "wb");
  if (out == nullptr) {
    std::perror(argv[2]);
    return 1;
  }
  trace::Header h{};
  std::memcpy(h.magic, trace::kMagic, 4);
  h.version = 1;
  h.ops = ops.size();
  h.ids = next_id;
  std::fwrite(&h, sizeof(h), 1, out);
  std::fwrite(ops.data(), sizeof(trace::Op), ops.size(), out);
  std::fclose(out);

  std::sort(alloc_sizes.begin(), alloc_sizes.end());
  auto pct = [&](double q) {
    return alloc_sizes.empty() ? 0 : alloc_sizes[static_cast<std::size_t>(q * static_cast<double>(alloc_sizes.size() - 1))];
  };
  std::printf(
      "{\"ops\":%zu,\"allocs\":%u,\"threads\":%llu,\"peak_live_mb\":%.1f,\"size_p50\":%llu,\"size_p90\":%llu,"
      "\"size_p99\":%llu,\"size_max\":%llu,\"untracked_frees\":%llu,\"implicit_frees\":%llu}\n",
      ops.size(), next_id, static_cast<unsigned long long>(threads), static_cast<double>(peak_bytes) / 1048576.0,
      static_cast<unsigned long long>(pct(0.5)), static_cast<unsigned long long>(pct(0.9)),
      static_cast<unsigned long long>(pct(0.99)), static_cast<unsigned long long>(pct(1.0)),
      static_cast<unsigned long long>(skipped_free), static_cast<unsigned long long>(implicit_free));
  return 0;
}
