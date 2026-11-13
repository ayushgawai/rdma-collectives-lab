# rdma-collectives-lab

Ring and tree all-reduce over RDMA verbs (Soft-RoCE) and TCP, plus a Linux char
driver with a DMA-style ring buffer. A small, single-machine lab for comparing
collective algorithms and transports — not a production collective library.

All ranks run as threads in one process. TCP uses real loopback sockets;
"RDMA" in the default build is a mock modelling RDMA semantics (registered
buffers, zero-copy pointer hand-off through QP-like bounded queues). A real
libibverbs transport skeleton lives in `rdma/verbs_transport.h` (Linux only,
not built by default — see Limitations).

## Design

```
                 all-reduce  (float sum, every rank ends with the total)
   ┌────────┐  ring:  reduce-scatter (P-1 steps) ──► all-gather (P-1 steps)
   │ rank 0 │──┐     chunk c sent to next rank, received chunk added/copied
   │ rank 1 │──┤  tree:  recursive-halving reduce to rank 0, then broadcast
   │ rank 2 │──┤     (full-vector sends; bandwidth factor is approximate)
   │ rank 3 │──┘
   └───┬────┘
       │  Transport interface: send_chunk(peer, ptr, n) / recv_chunk(peer) -> borrowed ptr
       ├──────────────────────────────┬────────────────────────────────┐
       │ Tcp                          │ MockRdma                       │
       │ full mesh of loopback        │ "MR" = stable registered buffer│
       │ sockets, 8-byte length hdr,  │ "QP" = bounded queue of        │
       │ recv into staging buffer     │ {ptr,n}; recv returns sender's │
       │ (one copy in)                │ pointer — zero copy            │
       └──────────────────────────────┴────────────────────────────────┘

   DMA-style ring (user-space twin in src/ringbuf_bench.cpp, kernel twin in driver/):
   ┌─ page 0: { atomic u64 head, u64 tail, u64 cap } ─ data[cap], cap = 2^k ─┐
   │  producer: claim_write() -> span, write in place, commit_write(n)     │
   │  consumer: claim_read()  -> span, read  in place, commit_read(n)      │
   │  monotonic head/tail masked by cap-1; no memcpy on the data path      │
   └────────────────────────────────────────────────────────────────────────┘
   driver/rdma_lab.c exposes the same contract as /dev/rdma_lab0:
   read/write stream through the ring, mmap = zero-copy, ioctl = stats/reset.
```

| Component | File | What it does |
|---|---|---|
| All-reduce bench | `src/allreduce.cpp` | Ring (reduce-scatter + all-gather, uneven-chunk safe) and binomial-tree all-reduce over the `Transport` interface; verifies the sum and reports algBW / busBW |
| TCP transport | `src/allreduce.cpp` (`Tcp`) | Full mesh of loopback sockets, length-prefixed chunks, 4 MiB socket buffers; sends run on a side thread so large messages don't deadlock send-before-recv |
| Mock-RDMA transport | `src/allreduce.cpp` (`MockRdma`) | Registered receive buffer + per-pair bounded queues handing off raw pointers — models Soft-RoCE zero-copy semantics in-process |
| Real verbs transport | `rdma/verbs_transport.h` | libibverbs skeleton: `ibv_reg_mr`, RC QP, signalled RDMA WRITE + CQ poll. Linux + `-DHAVE_RDMA` only; QP bring-up is **not** wired up (see Limitations) |
| Ring-buffer bench | `src/ringbuf_bench.cpp` | mmap SPSC ring, claim/commit zero-copy protocol, byte-pattern verification, GB/s report |
| Kernel driver | `driver/rdma_lab.c` | misc char device `/dev/rdma_lab0`, 1 MiB ring, `read`/`write`/`mmap`/`poll`, stats + reset ioctls |
| Tests | `tests/test_lab.py` | Correctness: ring & tree × TCP & mock-RDMA, 5 ranks, uneven size, ring-buffer stream |
| Benchmarks | `scripts/bench.sh`, `scripts/bench_mpi.sh` | Size sweep for both algos/transports; Open MPI comparison where `mpirun` exists |

## Run / test / bench

```sh
make            # build build/allreduce_bench and build/ringbuf_bench (clang++ -O3)
make test       # pytest if available, else the built-in runner in tests/test_lab.py
make bench      # size sweep (1 MiB / 16 MiB / 64 MiB) + ring-buffer bench + MPI check

./build/allreduce_bench --transport tcp  --algo ring --ranks 4 --size 16777216 --iters 3 --port 29400
./build/allreduce_bench --transport rdma --algo tree --ranks 4 --size 16777216 --iters 3
./build/ringbuf_bench --bytes 268435456 --runs 3
python3 scripts/driver_smoke.py --stress 10000   # mmap simulation off-Linux; /dev/rdma_lab0 on Linux
make -C driver                                   # Linux only, needs kernel headers
```

Requires a C++17 compiler and Python 3. No third-party dependencies
(pytest is optional; the test file runs standalone without it).

## Measured results

Measured 2026-11-12 on the development laptop (macOS, arm64, single process,
loopback / in-process transports). `make test`:

```
PASS test_ring_rdma_mock_correct
PASS test_ring_tcp_correct
PASS test_ring_uneven_size
PASS test_ringbuf_zero_copy_stream
PASS test_tree_rdma_mock_correct_ranks5
PASS test_tree_tcp_correct
6 passed
```

(The same 6 tests also pass under pytest: `6 passed`. A 10,000-run ring
stress passed at build time. The kernel-driver QEMU stress was **not** run —
no Linux box was available; see Limitations.)

`make bench` (algBW = bytes/time; busBW = algBW × 2(P−1)/P for ring,
algBW × 2 for tree — an approximation):

```
{"transport":"tcp","algo":"ring","ranks":4,"bytes":1048576,"iters":3,"correct":true,"seconds":0.0035,"algBW_GBps":0.892,"busBW_GBps":1.338}
{"transport":"tcp","algo":"tree","ranks":4,"bytes":1048576,"iters":3,"correct":true,"seconds":0.0035,"algBW_GBps":0.911,"busBW_GBps":1.822}
{"transport":"rdma","algo":"ring","ranks":4,"bytes":1048576,"iters":3,"correct":true,"seconds":0.0013,"algBW_GBps":2.503,"busBW_GBps":3.754}
{"transport":"tcp","algo":"ring","ranks":4,"bytes":16777216,"iters":3,"correct":true,"seconds":0.0459,"algBW_GBps":1.097,"busBW_GBps":1.645}
{"transport":"tcp","algo":"tree","ranks":4,"bytes":16777216,"iters":3,"correct":true,"seconds":0.0378,"algBW_GBps":1.332,"busBW_GBps":2.663}
{"transport":"rdma","algo":"ring","ranks":4,"bytes":16777216,"iters":3,"correct":true,"seconds":0.0095,"algBW_GBps":5.305,"busBW_GBps":7.957}
{"transport":"rdma","algo":"tree","ranks":4,"bytes":16777216,"iters":3,"correct":true,"seconds":0.0139,"algBW_GBps":3.624,"busBW_GBps":7.248}
{"transport":"tcp","algo":"ring","ranks":4,"bytes":67108864,"iters":3,"correct":true,"seconds":0.1493,"algBW_GBps":1.349,"busBW_GBps":2.023}
{"transport":"tcp","algo":"tree","ranks":4,"bytes":67108864,"iters":3,"correct":true,"seconds":0.1353,"algBW_GBps":1.488,"busBW_GBps":2.976}
{"transport":"rdma","algo":"ring","ranks":4,"bytes":67108864,"iters":3,"correct":true,"seconds":0.0416,"algBW_GBps":4.836,"busBW_GBps":7.254}
{"transport":"rdma","algo":"tree","ranks":4,"bytes":67108864,"iters":3,"correct":true,"seconds":0.0467,"algBW_GBps":4.312,"busBW_GBps":8.625}
{"bytes":268435456,"cap":4194304,"runs":3,"correct":true,"GBps":3.804,"note":"mmap SPSC, zero memcpy on data path"}
{"mpi":"skipped: mpirun not found on this machine"}
```

Read these as loopback/in-process upper bounds and an algorithm comparison,
not as RDMA hardware numbers: the mock transport moves pointers, not bytes
over a wire. (Full sweep output, including 1 MiB mock-RDMA rows, is reproduced
by `make bench`.)

## Limitations (honest)

- **No real RDMA run.** The default "rdma" transport is an in-process mock.
  `rdma/verbs_transport.h` registers memory and posts RDMA WRITEs, but QP
  bring-up (INIT→RTR→RTS, qpn/psn/gid exchange) is explicitly omitted there,
  it is not compiled by the default build, and there is no `make rdma` target.
  Nothing here has touched Soft-RoCE (`rxe`) or physical RoCE hardware.
- **Kernel driver untested.** `driver/rdma_lab.c` was written on macOS and has
  never been compiled against kernel headers, loaded, or run under QEMU.
  `driver/README-qemu.md` documents the intended test; `scripts/driver_smoke.py`
  on this machine only exercises the user-space mmap twin. Driver issues to
  expect on first load include the non-page-aligned `kmalloc` base used with
  `remap_pfn_range`/`virt_to_phys` and head/tail being shared with user space
  without barriers.
- **Single process, single node.** All "ranks" are threads; TCP is loopback
  only. No multi-host, no failure handling — a dead peer hangs or `exit(1)`s.
- **Tree busBW is approximate** (fixed ×2 factor); the tree sends full vectors
  each step, so it moves more data than ring for large messages.
- Correctness checks sample every 997th element, not the full vector.
- Benchmarks are best-of-short-runs on a laptop; expect wide variance.
