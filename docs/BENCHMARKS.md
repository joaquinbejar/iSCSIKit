# Benchmarks

Sequential throughput of the ASUSTOR AS6508T LUN (4 TB, mutual CHAP) over
gigabit Ethernet, Apple Silicon, macOS 26.6. Two layers are measured
separately so the cost of each is visible:

- **Transport** — `iscsikitd bench`, libiscsi talking straight to the target,
  no driver in the path. This is the ceiling the network and NAS allow.
- **Full stack** — `dd` against the block device the driver presents
  (`/dev/rdiskN`), i.e. kernel → dext → daemon → libiscsi → NAS.

All runs are queue depth 1, which is what the serial task pump gives the
kernel today. 256 MiB per I/O size for the transport runs.

## Transport (libiscsi direct, no dext)

| I/O size | Read MiB/s | Write MiB/s | Read avg | Write avg |
|---------:|-----------:|------------:|---------:|----------:|
| 16 KiB   | 15.6       | 12.3        | 1.00 ms  | 1.27 ms   |
| 64 KiB   | 28.8       | 26.7        | 2.17 ms  | 2.34 ms   |
| 256 KiB  | 61.3       | 49.6        | 4.08 ms  | 5.04 ms   |
| 1 MiB    | 83.5       | 46.5        | 11.98 ms | 21.52 ms  |

Reads reach ~83–89 MiB/s at 1 MiB I/O, the expected gigabit ceiling at queue
depth 1. Writes track reads up to 256 KiB; the 1 MiB write figure varies with
the NAS commit cadence. After each write batch the tool reads back every
region it wrote and compares it to the pattern, aborting on any mismatch.

## Full stack (dext block device, `dd if=/dev/rdiskN`)

| Daemon pump | Workload | Throughput | Tasks/s | Tasks in flight |
|---|---|---:|---:|---:|
| serial, sync libiscsi (build 22) | one `dd`, any `bs` | ~2.1 MB/s | ~130 | 1 |
| async event loop (build 28) | one `dd bs=1m` | 9.65 MB/s | ~590 | 1 |
| async event loop (build 29) | 8 parallel `dd bs=1m`, 128 MiB each | **47.3 MB/s** aggregate | up to 4245 | 8 |

Measured live with `iostat` and with the pump's own counters (printed every
5 s to `~/Library/Logs/iSCSIKit/daemon.log`): dequeue + complete cost
0.05–0.2 ms per task, zero transport errors during the parallel run.

Writes cannot be measured through the stack at all: on Apple Silicon macOS 26
the kernel stages a zero-filled buffer for outbound transfers
([WRITE-PATH-INVESTIGATION.md](WRITE-PATH-INVESTIGATION.md)), and the daemon's
read-only policy rejects every write before it reaches the target regardless.

## Reading the numbers

**A task is one page, and that is a framework limit.** `SCSIUserParallelTask`
carries a single `fBufferIOVMAddr` and no scatter-gather list; a virtual
controller has no DART to make a multi-page user buffer IOVM-contiguous.
Declaring more than one segment per command (builds 25/26) made every request
larger than one page fail with EIO in the kernel before it reached the dext.
So the dext reports one segment of 16 KiB (one Apple Silicon page), and the
kernel breaks larger requests into 16 KiB tasks.

**Throughput therefore comes from tasks in flight.** The serial pump waited
for each command (~1.7 ms round trip including two IOKit calls), which capped
everything at ~130 tasks/s. The async pump keeps as many commands outstanding
as the kernel hands it: one `dd` on the raw device is inherently queue depth 1
(the block layer's breaker issues the 16 KiB pieces of each `read()` one
after another), which gives 9.65 MB/s; eight concurrent readers reach 8 in
flight and 47 MB/s, over half the transport ceiling, with the per-task IPC
now at 0.05 ms. Filesystem reads with read-ahead and multiple processes
behave like the second case, not the first.

The transport writes succeed and verify while the identical CDBs fail
through the dext with a zeroed payload, which is direct evidence that the
write defect lives in the kernel's DriverKit staging, not in iSCSIKit's
transport.

## Reproducing

Transport (needs Local Network permission for the launching terminal; the
first run prompts):

```sh
cd Daemon && swift build -c release
LIBISCSI_CHAP_TARGET_USERNAME=<user> LIBISCSI_CHAP_TARGET_PASSWORD=<pass> \
  .build/release/iscsikitd bench 'iscsi://<user>%<pass>@<host>:3260/<iqn>/<lun>' --mib 256
# add --write --lba <START> to also measure writes into an unused region
```

Full stack (LUN presented as a disk by the app), read only:

```sh
for p in 16k:131072 64k:32768 256k:8192 1m:2048; do
  bs=${p%%:*}; cnt=${p##*:}; printf '%-5s ' "$bs"
  sudo dd if=/dev/rdiskN of=/dev/null bs=$bs count=$cnt 2>&1 | tail -1
done
```
