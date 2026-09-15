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

| I/O size | Read MiB/s |
|---------:|-----------:|
| 16 KiB   | ~1.5       |

Measured live with `iostat`: ~93 tps × 16 KiB. Larger `dd` block sizes do not
help, because the dext caps each transfer at `kMaxTransferSize` (16 KiB) and
the kernel issues those transfers serially.

Writes cannot be measured through the stack at all: on Apple Silicon macOS 26
the kernel stages a zero-filled buffer for outbound transfers
([WRITE-PATH-INVESTIGATION.md](WRITE-PATH-INVESTIGATION.md)), and the daemon's
read-only policy rejects every write before it reaches the target regardless.

## Reading the numbers

The same 16 KiB I/O costs **1.00 ms** at the transport but **~10.7 ms** through
the full stack — roughly a 10× tax per task, so full-stack read is ~1.5 MiB/s
against the transport's 15.6 MiB/s. The bottleneck is not the NAS or the
network (both idle at < 0.5 ms RTT) but the per-task round trip
kernel → dext → daemon (async IOUserClient notify, struct copy, `CreateMapping`)
→ synchronous libiscsi call → back.

That the transport writes succeed and verify, while the identical CDBs fail
through the dext with a zeroed payload, is direct evidence that the write
defect lives in the kernel's DriverKit staging, not in iSCSIKit's transport.

Two levers would raise full-stack throughput, both independent of the write
defect:

1. **Larger `kMaxTransferSize`** (16 KiB → 128 KiB / 512 KiB / 1 MiB). At the
   measured ~10.7 ms/op, 512 KiB per op would already give ~46 MiB/s.
2. **Queue depth > 1** — letting several tasks be outstanding multiplies
   throughput on top of (1).

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
