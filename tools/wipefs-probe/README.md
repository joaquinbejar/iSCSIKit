# wipefs-probe

A standalone copy of libutil's `wipefs_alloc()` + `wipefs_wipe()` with logging
on every call. Apple DTS suggested this instrumentation (case 22255765) to find
out why `diskutil eraseDisk` fails on an iSCSIKit LUN with

```
DMToolEraseDisk wipeDisk IntErr=45=0x2d aft wipefs_wipe err=45
DMToolEraseDisk wipeDisk DMErr=-69825 retVal=-69825
```

The probe shows which `pwrite()` inside `wipefs_wipe()` fails, what it
returned, and what `errno` was at that moment.

## Provenance

The copied logic comes from https://github.com/apple-oss-distributions/libutil,
branch `main`:

| item | value |
|---|---|
| branch head when fetched (2026-09-23) | `0f4264d617b0a94d8905521fa444e2aa215f0a4d` (libutil-73) |
| last commit touching `wipefs.cpp` | `490b68051c46597b761a68cd2b43d21dee9432cd` (libutil-70.1.1, 2023-09-22) |
| last commit touching `ExtentManager.cpp` | `e916a2b2f19be81ebd1b7ce6432bc27ef9bf6de6` (libutil-30) |
| `wipefs.cpp` sha256 | `fc948607abf3a02984568bc57747e031dcaf72161126a7eb87d5f46d9f048b2c` |
| `wipefs.h` sha256 | `da725c8b798850e71a0995eef74ac4b94a22a96ce6d33c87dbb470b23b02c011` |
| `ExtentManager.cpp` sha256 | `722fb93a7c121505522e1335c546fd085200ccfd2180e968490370671ad142b4` |
| `ExtentManager.h` sha256 | `30f87eac22ca28c8f67c3a1c7b1020cc0f0f9e33da5d8add1d287b06f1167fbc` |

**The published source is not the installed binary.** On the Mac where this
was written (macOS 27.0, 26A428), `/usr/lib/libutil.dylib` lives in the dyld
shared cache and reports `LC_SOURCE_VERSION 76.0` with UUID
`38245F99-D78C-38F1-94EE-22DF75E72ADE` (`dyld_info -load_commands -uuid
/usr/lib/libutil.dylib`). The newest published tag is libutil-73. The copied
logic was checked against that installed binary three ways:

1. **Disassembly** (`dyld_info -disassemble /usr/lib/libutil.dylib`). The
   installed build uses the same ioctls, the same extents, `bufSize = 0x20000`
   and the same loop. The only difference found is that the installed
   `wipefs_wipe` returns `EINVAL` for a NULL handle.
2. **`--selftest`.** The probe calls the installed `wipefs_alloc()` on a
   regular file and compares its extent list with its own copy. They match for
   `block_size` 0, 512 and 4096.
3. **Dynamic trace.** This was a one-off check in a scratch directory and is
   not part of this tool. `pwrite` and `ioctl` were interposed around the
   installed `wipefs_wipe()` on a 16 GiB sparse regular file with
   `block_size = 512`. The 33 `pwrite` calls it issued (offset and length)
   match `--simulate 512:33554432` exactly.

The failure itself was measured on macOS 26.6.2 (25G83), and that libutil
build was never inspected. The probe prints the UUID and source version of the
libutil it finds at run time, and says whether it is the binary checked above.

## What wipefs does (published source; line numbers are for `wipefs.cpp`)

`wipefs_alloc(fd, block_size, &h)` (lines 252-335):

- `fstat`. For `S_IFCHR`/`S_IFBLK` it calls `ioctl(DKIOCGETBLOCKSIZE)` into a
  `uint32_t nativeBlockSize` and `ioctl(DKIOCGETBLOCKCOUNT)` into a
  `uint64_t numBlocks`, then sets `totalSizeInBytes = numBlocks *
  nativeBlockSize`. It also records `diskN` for xartutil. For `S_IFREG` it
  uses `st_blksize` and `st_size`. It calls no other ioctl, and in particular
  not `DKIOCGETPHYSICALBLOCKSIZE`.
- `block_size == 0` means `nativeBlockSize`. storagekitd passes the
  `DKIOCGETBLOCKSIZE` value (see below), so the two are equal on a whole disk.
- `ExtentManager::Init(block_size, nativeBlockSize, totalBytes)` sets
  `totalBlocks = howmany(totalBytes, blockSize)` and adds two empty sentinel
  extents, at block 0 and at block `totalBlocks`.
- Byte ranges are added for these filesystems. Each end-of-disk range is
  computed from `totalBytes` and clipped to `[0, totalBlocks]` by
  `AddBlockRangeExtent`.

  | filesystem | ranges added |
  |---|---|
  | FutureFS | first 2 MiB, last 2 MiB |
  | HFS | `[0,1536)`, last 1 KiB |
  | MSDOS | first 32 KiB |
  | NTFS | first 32 KiB, last 32 KiB, `totalBytes/2 - 32 KiB` +64 KiB |
  | UDF | 32 KiB +14 KiB; blocks 256, 512, `last`, `last-256`; and when blockSize != 2048: byte 512 KiB, 1 MiB, `total-2048`, `total-512 KiB` (2 KiB each) |
  | UFS | 8 KiB +8 KiB |
  | ZFS | first and last 512 KiB |
  | Partitions | first 17 KiB (34 sectors), last 16.5 KiB (33 sectors) |
  | CoreStorage | first and last 512 B |

  The extents are merged and kept sorted by address.

`wipefs_wipe(h)` (lines 369-449):

1. If the disk has a name, it runs `/usr/sbin/xartutil --erase-disk diskN`.
   The probe skips this step.
2. `ioctl(DKIOCUNMAP, {offset 0, length totalBytes, kIOStorageUnmapOptionWhole})`.
   **The return value is ignored, and so is the errno it leaves behind.**
3. `bufSize = max(128 KiB, nativeBlockSize)`, zero-filled.
4. For each extent in ascending order: convert blocks to bytes, round out to
   `nativeBlockSize` boundaries, and clip the end at `totalBytes`. Then write
   `min(remaining, bufSize)` chunks:
   ```c
   if (pwrite(fd, bufZero, numBytesToWrite, byteOffset) != (ssize_t)numBytesToWrite) {
       err = errno;          // errno is only meaningful if pwrite returned -1
       goto labelExit;
   }
   ```
   A short count or an over-long count stops the wipe just like `-1` does. In
   both of those cases `errno` still holds whatever the last failing call left
   there, which is usually the ignored `DKIOCUNMAP`.
5. `labelExit`: `ioctl(DKIOCSYNCHRONIZECACHE)`, whose result is ignored. Then
   it returns `err`.

For the 16 GiB LUN (512 B x 33554432) the merged extents are `[0, 2 MiB)`,
`[8 GiB - 32 KiB, +64 KiB)` and `[16 GiB - 2 MiB, 16 GiB)`. That gives 33
pwrites: 16 x 128 KiB, then 1 x 64 KiB (the only partial chunk), then 16 x
128 KiB, with the last one ending exactly at `totalBytes`. At a 16 KiB
`IOMaximumByteCountWrite` that is 260 SCSI WRITEs. Run
`--simulate 512:33554432 --split-bytes 16384` to print the full plan.

## What storagekitd does around it (macOS 27.0 binary, not the 26.6.2 one)

The line DTS asked for, `blockSize=...`, is **not** in libutil. It is logged by
`-[DMToolEraseDisk wipeDisk:shared:]` in `/usr/libexec/storagekitd` (source
version 1076.0.4), found with `dyld_info -arch arm64e -disassemble
/usr/libexec/storagekitd`. The steps are:

1. It opens `/dev/r<disk>` with `O_RDWR`, or `O_RDWR|O_SHLOCK` when `shared`,
   and logs `open(2) out=(fd/-1=err)=%d errno=%d=%s`.
2. `DKIOCGETBLOCKSIZE`. On a whole disk it then calls `DKIOCGETBLOCKCOUNT`.
   For a partition it reads the "Size" from MediaKit instead.
3. It logs **`blockSize=%d blockCount=%lld totalBytes=%lld fd=%d`**.
4. It calls `wipefs_alloc(fd, blockSize, &h)`, then `wipefs_wipe(h)`, and logs
   `aft wipefs_wipe err=%d`.
5. If `err != 0`, it writes one zero block at offset 0 and one at `totalBytes -
   blockSize`. It logs `canWriteFirst=%d errno=%d` and `canWriteLast=%d
   errno=%d`, where `canWrite = (ret != -1)`. It reads errno only when ret is
   -1 and never checks the byte count.
6. `retVal = -69879 + 0x36 = -69825`. The only exception is first-write-OK
   with last-write-failed, which gives `-69879 + 0x77`.

This matches the observed log: `err=45`, `canWriteFirst=1`, `canWriteLast=1`,
then `-69825`. It also matches the daemon trace: the two single-block writes
at LBA 0 and LBA 33554431 are storagekitd's step 5, not part of wipefs.

## Build

```sh
tools/wipefs-probe/build.sh                 # -> tools/wipefs-probe/build/wipefs_probe (git-ignored)
tools/wipefs-probe/build.sh /some/out/dir   # or choose the output directory
```

This needs Xcode or the Command Line Tools (`clang++ -std=c++17`, IOKit,
CoreFoundation, `-lutil`). The probe never calls into libutil except in
`--selftest`, and even there only `wipefs_alloc`/`wipefs_free` on a regular
file.

## Usage

Start with a dry run. It is read-only: the device is opened `O_RDONLY`, only
getter ioctls are sent, and no `DKIOCUNMAP` or `DKIOCSYNCHRONIZECACHE` is
issued.

```sh
diskutil list                                   # find the 16 GiB LUN; confirm size 17.2 GB
sudo tools/wipefs-probe/build/wipefs_probe --device /dev/rdiskN --log ~/wipefs-probe-dry.log
```

The dry run prints:

- IORegistry facts for `diskN`: the IOMedia `Size` and `Preferred Block Size`,
  and, walking up the parents to the controller, `IOMaximumByteCount*`,
  `IOMaximumSegment*`, `Device Characteristics` and the block storage
  `Statistics`. This lets you verify the sizes DTS asked about against the
  registry.
- The same numbers as the BSD layer returns them: `DKIOCGETBLOCKSIZE`,
  `DKIOCGETBLOCKCOUNT`, `DKIOCGETPHYSICALBLOCKSIZE`, `DKIOCGETMAXBYTECOUNT*`,
  the segment limits, and `DKIOCGETFEATURES` (`DK_FEATURE_UNMAP`).
- The line `STORAGEKITD-EQUIVALENT blockSize=... blockCount=... totalBytes=...`.
- Every extent (raw, realigned, clipped), and every pwrite with its offset,
  length, end and LBA range. Each pwrite is tagged `full-chunk` or
  `PARTIAL-CHUNK`, and flagged `!!BEYOND-totalBytes` or
  `!!BEYOND-IOMedia-Size` when that applies.

Write mode is **destructive**. Use it on the disposable 16 GiB LUN only:

```sh
diskutil unmountDisk /dev/diskN
sudo tools/wipefs-probe/build/wipefs_probe --device /dev/rdiskN --write --i-understand-this-erases \
     --storagekitd-followup --log ~/wipefs-probe-write.log
```

For each pwrite, write mode logs the requested length, the offset, the return
value, `errno_after` (errno is set to 0 just before the call) and
`errno_before_reset`. It classifies the call as `OK`, `SHORT`,
`OVERLONG(ret>len)` or `ERROR(-1)`. At the first mismatch it stops, as wipefs
does, and prints the errno wipefs would have returned and where that errno
came from: the pwrite itself, or the stale value left by the ignored
`DKIOCUNMAP`. A mismatch always counts as a failure, even when errno was 0
(wipefs would then return 0): the `WIPE result` and `DONE` lines say
`COUNT MISMATCH`, `--storagekitd-followup` runs, and the exit status is 11
(10 when the wipefs err is nonzero). The `DKIOCUNMAP` and `DKIOCSYNCHRONIZECACHE` results are logged
separately. The IORegistry is dumped before and after the run, so you can
compare the `Statistics` byte counters with the bytes actually requested.

Other modes:

- `--simulate BS:COUNT` needs no device at all.
- `--selftest FILE` compares the probe's extents with the installed libutil.
- `--provenance` prints the provenance and the installed libutil version.

All arguments are parsed and `--log` is opened before any mode runs. Numbers
are decimal or `0x` hex; a sign or a leading `0` (octal) is rejected.
`--wipefs-block-size` must be a power of two in [512, 2^31].

Guards:

- Size: the probe refuses any device over 64 GiB unless `--allow-large` is
  given. This protects the 4 TB LUN.
- Write mode: requires both `--write` and `--i-understand-this-erases`, and
  only accepts a whole-disk `/dev/rdiskN`. It refuses media that
  `DKIOCISWRITABLE` reports as not writable, and a disk with a mounted volume:
  either a slice of it, or a volume stacked on it (an APFS volume `diskMsK`
  whose container's physical store is a slice of `diskN`, found by walking
  the IORegistry parents of every mounted volume).
- iSCSIKit only: write mode refuses a disk whose IORegistry ancestry has no
  iSCSIKit / `IOUserSCSIParallelInterfaceController` entry (the
  `IOREG iSCSIKit ancestor:` line), unless `--allow-non-iscsikit` is given.

For the writes to reach the target at all, the daemon must be allowed to write
to that target. Otherwise its read-only policy rejects them before the
transport. Enable `traceTasks` too, so that the daemon trace can be lined up
with the probe log.

## Reading the result

| probe output for the failing pwrite | meaning |
|---|---|
| `ERROR(-1)` with errno 45 | the write path itself returned ENOTSUP |
| `SHORT` or `OVERLONG`, errno_after 0, "STALE errno left by the ignored DKIOCUNMAP" | the byte count the stack reported is wrong; the 45 in the storagekitd log is the stale `DKIOCUNMAP` errno |
| `COUNT MISMATCH` with err=0 | wipefs would return 0 although its count check failed; the probe still reports failure (exit 11) |
| every pwrite `OK` | the wipe succeeds outside storagekitd; compare the environments (xartutil, `O_SHLOCK`, timing) |

**Identified cause (fixed in 18d414b and d552100; still to be confirmed on
hardware with this probe).**

Before 18d414b every WRITE completed with `fBytesTransferred` = 1 MiB,
whatever its size:

1. `DextClient.dequeueTask` (Daemon/Sources/iscsikitd/DextClient.swift:101-124)
   passes an output buffer of `sizeof(descriptor) + ISCSIKIT_MAX_TRANSFER`
   (1 MiB). That is more than 4096 bytes, so IOKit sends it out-of-line
   (IOKitUser `IOKitLib.c:1875-1897`), and the dext fills
   `structureOutputDescriptor` (Dext/iSCSIKitDext.cpp:398-415). DriverKit's
   `IOUserClientMethodArguments` has no field for reporting the size written.
2. xnu `is_io_connect_method` returns `*ool_output_size =
   args.structureOutputDescriptorSize`, which is the caller's full size
   (`iokit/Kernel/IOUserClient.cpp:5206-5222`, xnu main `f6217f8`), so the
   daemon saw `dataOut.count == 1048576` for every task and reported it as
   bytes written.
3. For raw I/O, IOMediaBSDClient sets `resid -= actualByteCount`
   (`IOMediaBSDClient.cpp:2825, 3241`). A 16 KiB dd write returned 1048576
   ("1048576 bytes transferred", "short write"), and a 128 KiB wipefs pwrite
   returned `!= 131072` with errno untouched, so wipefs returned the stale
   `DKIOCUNMAP` errno 45 and diskutil failed with -69825.

What the code does now:

- Daemon: the payload is `min(dataOut.count, transferLength)`
  (SessionPump.swift:324), and a write reports only what the target accepted:
  0 unless the status is GOOD, otherwise that payload minus libiscsi's
  underflow residual (SessionPump.swift:360-364).
- Dext: a write payload reaches the daemon whole or the dequeue fails
  (iSCSIKitDext.cpp:386-394, 407-411), and the completion reports
  `min(reply.bytesTransferred, fRequestedTransferCount)`, for reads only the
  bytes actually copied (iSCSIKitDext.cpp:465-513).

So on dext build 34 or later the probe should show no `OVERLONG`. If one
still appears, a byte count is inflated somewhere the clamps do not cover
(another path above the dext, or a stale dext still loaded; check the build
with `systemextensionsctl list`). A `SHORT` now means the target accepted
fewer bytes than requested: a CHECK CONDITION or a residual on that WRITE,
which the daemon trace (`traceTasks`) shows for the same LBA, and errno then
tells whether the error reached the syscall. Either way the probe reports the
run as failed.

The probe's write mode confirms or refutes this with one number: the return
value of pwrite #1.

## License

The ExtentManager class, the `AddExtentsFor*()` functions, `query_disk_info()`
and the wipefs computations and loop are copied from Apple's libutil under the
Apple Public Source License 2.0 (http://www.opensource.apple.com/apsl/). The
license header is reproduced in `wipefs_probe.cpp`, and every modification is
marked `PROBE:`. The rest of the file is part of iSCSIKit.
