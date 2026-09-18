# RAMDisk reproducer for FB24799838 (DTS Case-ID 22255765)

A software-only SCSIControllerDriverKit controller with one 64 MiB RAM-backed
LUN and no hardware, network or helper process. Every WRITE it receives logs
how many bytes of the buffer returned by `UserGetDataBuffer` are nonzero.

Validation status: this standalone reproducer has not yet been verified on
the affected Mac. Its dext remained stuck in state U during the attempt;
validation after a reboot is pending. The measurements below come from the
main iSCSIKit dext, not this RAMDisk.

## Build

```sh
brew install xcodegen      # or open the included RAMDiskRepro.xcodeproj
xcodegen generate
open RAMDiskRepro.xcodeproj
```

Set your team in `project.yml` / Signing. The dext needs the DriverKit and
SCSIController entitlements (development). Build the `RAMDiskRepro` scheme,
copy the app to /Applications, launch it, click **Install Driver**, approve in
System Settings.

## Reproduce

IMPORTANT: exercise it with `diskutil eraseDisk`, not with a raw `dd` to
`/dev/rdiskN`. In the main dext, one raw write delivered nonzero data, while
all 60 writes from `diskutil eraseDisk` arrived zero-filled. See
[experiment 10](../docs/WRITE-PATH-INVESTIGATION.md#experiment-10-2026-09-18-the-writes-origin-is-the-discriminator).
Raw writes are a useful control, but cannot alone validate the failing path.

```sh
diskutil list                                   # "RAMDisk repro" 64 MB disk appears as diskN
sudo log stream --predicate 'eventMessage CONTAINS "RAMDiskDext: WRITE"' &
sudo diskutil eraseDisk APFS T diskN             # select only the disposable RAMDisk
```

Expected: the log lines report nonzero byte counts (the GPT headers and the
filesystem structures being written are not zero).
Failure signature to check: `WRITE lba … : 0 nonzero bytes in the buffer from
UserGetDataBuffer` for writes that should contain GPT or filesystem data.
Record the formatting result and readback; this signature is not yet
confirmed in the standalone reproducer. Nonzero-byte counts alone do not
establish byte-for-byte integrity.

Source: `RAMDiskDext/RAMDiskDext.cpp` (the check is in the `rw` lambda).
