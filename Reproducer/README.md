# RAMDisk reproducer for FB24799838

A software-only SCSIControllerDriverKit controller with one 64 MiB RAM-backed
LUN and no hardware, network or helper process. Every WRITE it receives logs
how many bytes of the buffer returned by `UserGetDataBuffer` are nonzero.

Observed on Apple Silicon macOS 26.6.2: the count is **0 for every write**
(the medium ends up zero-filled), while READ through the same buffer works.

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

```sh
diskutil list                                   # "RAMDisk repro" 64 MB disk appears as diskN
sudo log stream --predicate 'eventMessage CONTAINS "RAMDiskDext: WRITE"' &
sudo diskutil eraseDisk JHFS+ T diskN          # or: sudo dd if=/dev/urandom of=/dev/rdiskN bs=16k count=4
```

Expected: the log lines report nonzero byte counts (the GPT headers and the
filesystem structures being written are not zero).
Actual: `WRITE lba … : 0 nonzero bytes in the buffer from UserGetDataBuffer`
for every write; reading the disk back returns zeros.

Source: `RAMDiskDext/RAMDiskDext.cpp` (the check is in the `rw` lambda).
