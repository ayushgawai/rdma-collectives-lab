#!/bin/sh
set -e
cd "$(dirname "$0")/.."
make -s all
for size in 1048576 16777216 67108864; do
  ./build/allreduce_bench --transport tcp  --algo ring --ranks 4 --size $size --iters 3 --port 29400
  ./build/allreduce_bench --transport tcp  --algo tree --ranks 4 --size $size --iters 3 --port 29500
  ./build/allreduce_bench --transport rdma --algo ring --ranks 4 --size $size --iters 3
  ./build/allreduce_bench --transport rdma --algo tree --ranks 4 --size $size --iters 3
done
./build/ringbuf_bench --bytes 268435456 --runs 3
sh scripts/bench_mpi.sh
