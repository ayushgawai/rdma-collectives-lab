# Testing the driver in QEMU (Linux only — the macOS dev laptop cannot load it)

```sh
make -C driver                       # needs kernel headers for the running kernel
# Minimal QEMU guest with a busybox initramfs:
qemu-system-x86_64 -kernel /boot/vmlinuz-$(uname -r) -initrd initramfs.cpio.gz \
  -append "console=ttyS0" -nographic -m 512
# in the guest:
insmod rdma_lab.ko
ls -l /dev/rdma_lab0
python3 ../scripts/driver_smoke.py    # write/read round-trip + ioctl stats + mmap check
```

`scripts/driver_smoke.py` is the same test that runs against the mmap
simulation on macOS (via `src/ringbuf_bench`), so the user-space contract is
tested everywhere; only the `insmod` step is Linux/QEMU-only. Stress target:
10,000 open/write/read/close cycles (`driver_smoke.py --stress 10000`) —
reported honestly in the top-level README from the last QEMU run, or marked
not-run if no Linux box was available.
