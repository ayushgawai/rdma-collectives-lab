// Ring / tree all-reduce over TCP (real loopback sockets) and Mock-RDMA
// (registered buffers, zero-copy pointer hand-off through QP-like queues).
// Real Soft-RoCE verbs transport skeleton lives in rdma/verbs_transport.h (Linux only).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

struct Transport {
  virtual ~Transport() = default;
  virtual void send_chunk(int peer, const float* p, size_t n) = 0;
  virtual const float* recv_chunk(int peer, size_t& n) = 0;  // borrowed ptr, valid until next recv from peer
  virtual const char* name() = 0;
};

// ---------------- Mock RDMA: MR = aligned allocation, QP = bounded queue of pointers.
struct MockRdma : Transport {
  struct Msg { const float* p; size_t n; };
  int P, rank;
  std::vector<std::vector<std::queue<Msg>>>* q;  // q[src][dst]
  std::vector<std::mutex>* mu; std::vector<std::condition_variable>* cv;
  std::vector<float> reg;  // "registered" receive buffer (pinned in real verbs)
  MockRdma(int P_, int r, decltype(q) q_, decltype(mu) m, decltype(cv) c, size_t maxn)
      : P(P_), rank(r), q(q_), mu(m), cv(c) { reg.resize(maxn); }  // stable address after resize = the "MR"
  void send_chunk(int peer, const float* p, size_t n) override {
    int idx = rank * P + peer;
    { std::lock_guard<std::mutex> g((*mu)[idx]); (*q)[rank][peer].push({p, n}); }
    (*cv)[idx].notify_one();
  }
  const float* recv_chunk(int peer, size_t& n) override {
    int idx = peer * P + rank; Msg m;
    { std::unique_lock<std::mutex> lk((*mu)[idx]);
      (*cv)[idx].wait(lk, [&] { return !(*q)[peer][rank].empty(); });
      m = (*q)[peer][rank].front(); (*q)[peer][rank].pop(); }
    n = m.n; return m.p;  // zero copy: reader adds straight out of sender's MR
  }
  const char* name() override { return "rdma-mock(Soft-RoCE model)"; }
};

// ---------------- TCP: full mesh of loopback sockets, registered staging buffer.
struct Tcp : Transport {
  int P, rank; std::vector<int>* fds; std::vector<float> stage;
  Tcp(int P_, int r, std::vector<int>* f, size_t maxn) : P(P_), rank(r), fds(f) { stage.resize(maxn); }
  int fd(int peer) { return (*fds)[rank * P + peer]; }
  void send_chunk(int peer, const float* p, size_t n) override {
    uint64_t hdr = n; const char* c = (const char*)&hdr; size_t left = 8;
    while (left) { ssize_t k = ::send(fd(peer), c, left, 0); if (k <= 0) { perror("send"); exit(1);} c += k; left -= k; }
    c = (const char*)p; left = n * 4;
    while (left) { ssize_t k = ::send(fd(peer), c, left, 0); if (k <= 0) { perror("send"); exit(1);} c += k; left -= k; }
  }
  const float* recv_chunk(int peer, size_t& n) override {
    uint64_t hdr; char* c = (char*)&hdr; size_t left = 8;
    while (left) { ssize_t k = ::recv(fd(peer), c, left, 0); if (k <= 0) { perror("recv"); exit(1);} c += k; left -= k; }
    n = hdr; c = (char*)stage.data(); left = n * 4;
    while (left) { ssize_t k = ::recv(fd(peer), c, left, 0); if (k <= 0) { perror("recv"); exit(1);} c += k; left -= k; }
    return stage.data();
  }
  const char* name() override { return "tcp"; }
};

static void bounds(size_t n, int P, int c, size_t& off, size_t& len) {
  size_t base = n / P, rem = n % P;
  off = c * base + std::min<size_t>(c, rem); len = base + (c < (int)rem ? 1 : 0);
}

// Send runs on a side thread: for large messages a blocking send-before-recv deadlocks.
static void xchg(Transport& t, int rank, int P, float* buf, size_t n, int sendc, int recvc, bool add) {
  size_t soff, slen, roff, rlen; bounds(n, P, sendc, soff, slen); bounds(n, P, recvc, roff, rlen);
  std::thread th([&] { t.send_chunk((rank + 1) % P, buf + soff, slen); });
  size_t rn; const float* src = t.recv_chunk((rank - 1 + P) % P, rn);
  if (add) for (size_t i = 0; i < rlen; i++) buf[roff + i] += src[i];
  else memcpy(buf + roff, src, rlen * 4);
  th.join();
}

static void ring(Transport& t, int rank, int P, float* buf, size_t n) {
  if (P == 1) return;
  for (int s = 0; s < P - 1; s++) xchg(t, rank, P, buf, n, (rank - s + 2 * P) % P, (rank - s - 1 + 2 * P) % P, true);
  for (int s = 0; s < P - 1; s++) xchg(t, rank, P, buf, n, (rank - s + 1 + 2 * P) % P, (rank - s + 2 * P) % P, false);
}

static void tree(Transport& t, int rank, int P, float* buf, size_t n) {
  std::vector<float> tmp(n);
  for (int stride = 1; stride < P; stride *= 2) {
    if (rank % (2 * stride) == stride) { t.send_chunk(rank - stride, buf, n); break; }
    if (rank % (2 * stride) == 0 && rank + stride < P) { size_t rn; const float* s = t.recv_chunk(rank + stride, rn); for (size_t i = 0; i < n; i++) buf[i] += s[i]; }
  }
  for (int stride = 1 << (31 - __builtin_clz(P)); stride >= 1; stride /= 2) {
    if (rank % (2 * stride) == 0 && rank + stride < P) t.send_chunk(rank + stride, buf, n);
    else if (rank % (2 * stride) == stride) { size_t rn; const float* s = t.recv_chunk(rank - stride, rn); memcpy(buf, s, n * 4); }
  }
}

int main(int argc, char** argv) {
  std::string transport = "tcp", algo = "ring"; int P = 4, iters = 5; size_t bytes = 1 << 20; int base_port = 29100; bool verify = true;
  for (int i = 1; i + 1 < argc; i += 2) { std::string k = argv[i], v = argv[i + 1];
    if (k == "--transport") transport = v; else if (k == "--algo") algo = v; else if (k == "--ranks") P = stoi(v);
    else if (k == "--size") bytes = stoull(v); else if (k == "--iters") iters = stoi(v); else if (k == "--port") base_port = stoi(v); }
  size_t n = bytes / 4;

  // TCP mesh
  std::vector<int> fds(P * P, -1);
  if (transport == "tcp") {
    std::vector<int> listeners(P);
    for (int r = 0; r < P; r++) { listeners[r] = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(listeners[r], SOL_SOCKET, SO_REUSEADDR, &one, 4);
      sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(base_port + r);
      if (bind(listeners[r], (sockaddr*)&a, sizeof(a)) || listen(listeners[r], P)) { perror("bind/listen"); return 1; } }
    std::vector<std::thread> acc;
    for (int r = 0; r < P; r++) acc.emplace_back([&, r] { for (int k = 0; k < r; k++) { int fd = accept(listeners[r], nullptr, nullptr); fds[r * P + k] = fd; } });
    for (int r = 0; r < P; r++) for (int peer = r + 1; peer < P; peer++) { int fd = socket(AF_INET, SOCK_STREAM, 0);
      sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(base_port + peer);
      if (connect(fd, (sockaddr*)&a, sizeof(a))) { perror("connect"); return 1; } fds[r * P + peer] = fd; }
    for (auto& t : acc) t.join(); for (int s : listeners) close(s);
    int bufsz = 4 << 20; for (int fd : fds) if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, 4), setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, 4);
  }
  std::vector<std::vector<std::queue<MockRdma::Msg>>> mq(P, std::vector<std::queue<MockRdma::Msg>>(P));
  std::vector<std::mutex> mu(P * P); std::vector<std::condition_variable> cv(P * P);
  std::atomic<int> errors{0}; double elapsed = 0;
  auto run_once = [&](bool timed) {
    std::vector<std::thread> th; auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < P; r++) th.emplace_back([&, r] {
      std::vector<float> buf(n);
      for (size_t i = 0; i < n; i++) buf[i] = (r + 1) * (1.0f + (i % 7) * 0.001f);
      std::unique_ptr<Transport> t;
      if (transport == "tcp") t = std::make_unique<Tcp>(P, r, &fds, n); else t = std::make_unique<MockRdma>(P, r, &mq, &mu, &cv, n);
      if (algo == "ring") ring(*t, r, P, buf.data(), n); else tree(*t, r, P, buf.data(), n);
      if (verify) { float want = 0; for (int k = 1; k <= P; k++) want += k;
        for (size_t i = 0; i < n; i += 997) if (std::fabs(buf[i] - want * (1.0f + (i % 7) * 0.001f)) > 0.05f) errors++; }
    });
    for (auto& t : th) t.join();
    if (timed) elapsed += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  };
  run_once(false);  // warm-up + correctness
  if (errors) { printf("{\"correct\":false,\"errors\":%d}\n", errors.load()); return 1; }
  for (int i = 0; i < iters; i++) run_once(true);
  double algbw = bytes * double(iters) / elapsed / 1e9;
  double busbw = algo == "ring" ? algbw * 2.0 * (P - 1) / P : algbw * 2.0;  // tree factor is an approximation, see README
  printf("{\"transport\":\"%s\",\"algo\":\"%s\",\"ranks\":%d,\"bytes\":%zu,\"iters\":%d,\"correct\":true,\"seconds\":%.4f,\"algBW_GBps\":%.3f,\"busBW_GBps\":%.3f}\n",
         transport.c_str(), algo.c_str(), P, bytes, iters, elapsed, algbw, busbw);
  return 0;
}
