// Zero-copy SPSC ring buffer over mmap — the user-space twin of driver/rdma_lab.c.
// Same contract: claim a span, write/read in place, commit. No memcpy on the data path.
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

struct Ring {
  struct Hdr { std::atomic<uint64_t> head{0}, tail{0}; uint64_t cap; };
  Hdr* h; uint8_t* data; size_t cap_mask; void* base; size_t maplen;
  static Ring create(size_t cap) {  // cap must be power of two
    Ring r; r.maplen = sizeof(Hdr) + cap;
    r.base = mmap(nullptr, r.maplen, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    r.h = (Hdr*)r.base; r.h->head = 0; r.h->tail = 0; r.h->cap = cap; r.data = (uint8_t*)r.base + sizeof(Hdr); r.cap_mask = cap - 1; return r;
  }
  // producer: returns writable span (may be < len at wrap / when full), commit(n) publishes it
  uint8_t* claim_write(size_t& len) { uint64_t t = h->tail.load(std::memory_order_relaxed), hd = h->head.load(std::memory_order_acquire);
    size_t free_ = (size_t)h->cap - (size_t)(t - hd); if (!free_) { len = 0; return nullptr; } len = std::min(len, std::min(free_, (size_t)h->cap - (size_t)(t & cap_mask))); return data + (t & cap_mask); }
  void commit_write(size_t n) { h->tail.store(h->tail.load(std::memory_order_relaxed) + n, std::memory_order_release); }
  uint8_t* claim_read(size_t& len) { uint64_t hd = h->head.load(std::memory_order_relaxed), t = h->tail.load(std::memory_order_acquire);
    size_t avail = (size_t)(t - hd); if (!avail) { len = 0; return nullptr; } len = std::min(len, std::min(avail, (size_t)h->cap - (size_t)(hd & cap_mask))); return data + (hd & cap_mask); }
  void commit_read(size_t n) { h->head.store(h->head.load(std::memory_order_relaxed) + n, std::memory_order_release); }
};

int main(int argc, char** argv) {
  size_t bytes = 256ull << 20, cap = 4ull << 20; int runs = 1;
  for (int i = 1; i + 1 < argc; i += 2) { std::string k = argv[i], v = argv[i + 1];
    if (k == "--bytes") bytes = stoull(v); else if (k == "--cap") cap = stoull(v); else if (k == "--runs") runs = stoi(v); }
  double best = 0; uint64_t expect_sum = 0;
  for (int run = 0; run < runs; run++) {
    Ring r = Ring::create(cap); std::atomic<uint64_t> sum{0}; std::atomic<bool> bad{false};
    auto t0 = std::chrono::steady_clock::now();
    std::thread prod([&] { uint64_t sent = 0; while (sent < bytes) { size_t want = std::min<size_t>(1 << 16, bytes - sent), len = want; uint8_t* p = r.claim_write(len);
        if (!p) { std::this_thread::yield(); continue; } for (size_t i = 0; i < len; i++) p[i] = uint8_t((sent + i) * 31 + 7); r.commit_write(len); sent += len; } });
    std::thread cons([&] { uint64_t got = 0; while (got < bytes) { size_t len = 1 << 16; uint8_t* p = r.claim_read(len);
        if (!p) { std::this_thread::yield(); continue; } uint64_t s = 0; for (size_t i = 0; i < len; i++) { if (p[i] != uint8_t((got + i) * 31 + 7)) bad = true; s += p[i]; } sum += s; r.commit_read(len); got += len; } });
    prod.join(); cons.join();
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    best = std::max(best, bytes / sec / 1e9); munmap(r.base, r.maplen);
    if (bad) { printf("{\"correct\":false}\n"); return 1; }
  }
  printf("{\"bytes\":%zu,\"cap\":%zu,\"runs\":%d,\"correct\":true,\"GBps\":%.3f,\"note\":\"mmap SPSC, zero memcpy on data path\"}\n", bytes, cap, runs, best);
  return 0;
}
