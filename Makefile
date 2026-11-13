CXX ?= clang++
CXXFLAGS ?= -O3 -std=c++17 -Wall -pthread -march=native
all: build/allreduce_bench build/ringbuf_bench
build/allreduce_bench: src/allreduce.cpp
	mkdir -p build && $(CXX) $(CXXFLAGS) $< -o $@
build/ringbuf_bench: src/ringbuf_bench.cpp
	mkdir -p build && $(CXX) $(CXXFLAGS) $< -o $@
test: all
	python3 -m pytest tests/ -q || python3 tests/test_lab.py
bench: all
	sh scripts/bench.sh
driver:
	$(MAKE) -C driver
clean:
	rm -rf build
.PHONY: all test bench driver clean
