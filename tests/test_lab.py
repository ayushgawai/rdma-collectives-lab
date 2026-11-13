import json, subprocess, sys
BIN = "build/allreduce_bench"
def run(*a):
    out = subprocess.check_output([BIN, *a], text=True, timeout=120)
    return json.loads(out.strip().splitlines()[-1])

def test_ring_tcp_correct(): assert run("--transport","tcp","--algo","ring","--ranks","4","--size","1048576","--iters","1","--port","29200")["correct"]
def test_tree_tcp_correct(): assert run("--transport","tcp","--algo","tree","--ranks","4","--size","1048576","--iters","1","--port","29300")["correct"]
def test_ring_rdma_mock_correct(): assert run("--transport","rdma","--algo","ring","--ranks","4","--size","1048576","--iters","1")["correct"]
def test_tree_rdma_mock_correct_ranks5(): assert run("--transport","rdma","--algo","tree","--ranks","5","--size","262144","--iters","1")["correct"]
def test_ring_uneven_size(): assert run("--transport","rdma","--algo","ring","--ranks","4","--size","999424","--iters","1")["correct"]

def test_ringbuf_zero_copy_stream():
    out = subprocess.check_output(["build/ringbuf_bench","--bytes","16777216","--runs","1"], text=True, timeout=120)
    d = json.loads(out); assert d["correct"] and d["GBps"] > 0.1

if __name__ == "__main__":
    for name, fn in sorted(list(globals().items())):
        if name.startswith("test_"): fn(); print("PASS", name)
