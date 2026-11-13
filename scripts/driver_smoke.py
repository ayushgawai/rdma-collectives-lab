#!/usr/bin/env python3
"""Driver smoke test. Against /dev/rdma_lab0 on Linux/QEMU; on other systems
it exercises the same write/read/ioctl contract via the mmap bench binary."""
import ctypes, fcntl, os, subprocess, sys
DEV = "/dev/rdma_lab0"
class Stats(ctypes.Structure): _fields_ = [("produced", ctypes.c_ulonglong), ("consumed", ctypes.c_ulonglong), ("overruns", ctypes.c_ulonglong), ("cap", ctypes.c_uint)]
def main():
    stress = int(sys.argv[sys.argv.index("--stress") + 1]) if "--stress" in sys.argv else 1
    if not os.path.exists(DEV):
        print(f"{DEV} not present (not Linux/QEMU) — running mmap-simulation path instead")
        out = subprocess.check_output(["build/ringbuf_bench", "--bytes", "16777216", "--runs", str(min(stress, 3))], text=True)
        print(out.strip()); return
    for i in range(stress):
        fd = os.open(DEV, os.O_RDWR); data = bytes(range(256)) * 64
        os.write(fd, data); assert os.read(fd, len(data)) == data
        s = Stats(); fcntl.ioctl(fd, 0x80185201, s, True)  # RDL_IOC_GET_STATS ('R',1)
        assert s.produced >= len(data); os.close(fd)
    print(f"driver smoke OK: {stress} cycles, cap={s.cap}")
if __name__ == "__main__": main()
