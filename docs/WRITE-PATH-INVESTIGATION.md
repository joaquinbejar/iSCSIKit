# iSCSIKit: Write Data Path Investigation Report

> Reported to Apple as Feedback **FB24799838** (2026-09-16).

Status: OPEN PROBLEM, updated 2026-09-18. Reads work end to end. One raw
write to `/dev/rdiskN` delivered nonzero payload, but all 60 measured writes
from `diskutil eraseDisk` arrived zero-filled. Formatting remains blocked.
Experiment 10 supersedes the earlier conclusion that every write fails.

## 1. Project context

iSCSIKit (https://github.com/joaquinbejar/iSCSIKit) is an open-source iSCSI
initiator for Apple Silicon Macs built on the sanctioned userspace driver
stack, with no kexts:

```
macOS storage stack (Disk Utility, APFS, /dev/diskN)
        │ SCSI CDBs (kernel-built)
iSCSIKitDext: virtual HBA, subclass of IOUserSCSIParallelInterfaceController
        │ IOUserClient (async task notify; dequeue/complete external methods)
iscsikitd: userspace daemon, libiscsi raw CDB passthrough over TCP
        │ TCP 3260
iSCSI target (ASUSTOR NAS, mutual CHAP, header/data digests)
```

The dext is a software-only controller: `IOProviderClass = IOUserResources`,
no PCI device, no hardware DMA engine. Every SCSI task the kernel dispatches
must have its data buffer accessed with the CPU so the daemon can move it
over TCP.

## 2. Environment

- Mac Studio, Apple Silicon (arm64, 16KB pages), 64 GB RAM
- macOS 26.6.2 (build 25G83), Darwin 25.6.0
- Xcode 26.6 (17F113), DriverKit SDK 25.5
- SCSIControllerDriverKit framework version 352 (from crash log binary images)
- DriverKit runtime 456.120.3
- Dext signed with Apple Development certificate, self-service development
  entitlements: `com.apple.developer.driverkit`,
  `com.apple.developer.driverkit.family.scsicontroller`,
  `com.apple.developer.driverkit.allow-any-userclient-access`
- Target: ASUSTOR AS6508T, 4 TB thin-provisioned LUN, 512-byte blocks,
  mutual CHAP

## 3. What works (verified end to end)

- Dext activation, replacement upgrades, IORegistry publication
  (`IOUserSCSIParallelInterfaceController` node, SCSI Parallel Domain in
  System Profiler).
- `UserCreateTargetForID` from a private dispatch queue; target device
  `IOSCSIParallelInterfaceDevice@0` appears and completes probe.
- Full READ path: INQUIRY (standard and EVPD), READ CAPACITY, READ(10/16),
  TEST UNIT READY, SYNCHRONIZE CACHE all complete with correct data. The
  LUN publishes as `/dev/diskN (external, physical): 4.4 TB` and matches
  the target byte for byte (verified against direct iSCSI reads).
- The read data path is: daemon returns data inline; dext copies it into
  the task's bounce buffer (obtained via `UserGetDataBuffer` +
  `GetAddressRange`); the kernel then delivers those bytes to the caller.
  This proves the bounce buffer mapping is shared bidirectionally between
  kernel and dext.

## 4. The problem

For the measured formatting WRITE tasks
(`fTransferDirection == kSCSIDataTransfer_FromInitiatorToTarget`), the
expected outbound payload is absent when inspected in the dext:

- `diskutil eraseDisk` reaches "Wiping volume data" and fails with -69825;
  GPT writes complete with GOOD status but reading back LBA 0 over iSCSI
  shows all zeros: the payload the daemon sent was zeros because that is
  all the dext ever saw.
- `sudo dd if=/dev/zero of=/dev/rdiskN bs=4k count=1` flows through the
  whole stack and returns success (payload is zeros anyway, so no way to
  distinguish). Writes with nonzero data (GPT headers) prove the loss.

## 5. Experiments performed (each verified on real hardware)

| # | Hypothesis | Test | Result |
|---|---|---|---|
| 1 | Bounce from `UserGetDataBuffer` carries write data | Copy from `GetAddressRange().address` at dequeue time | All zeros (`nz 0` on every write, GPT headers included) |
| 2 | Data staged later | Re-fetch `UserGetDataBuffer` at dequeue (later, different context) and copy from the fresh buffer | Still all zeros |
| 3 | Data at a page offset inside the bounce | Scan first 64 KB of the bounce for any nonzero byte at `UserProcessParallelTask` time | No nonzero byte at any offset (`stage 0xffff`) |
| 4 | `fBufferIOVMAddr` is dereferenceable in a virtual dext | `memcpy` from it | SIGSEGV, KERN_INVALID_ADDRESS; address is not in any VM region of the dext (it is a DMA/IOVA address) |
| 5 | "The caller will have to prepare new DMA mappings for this buffer" (header doc) triggers copy-in | `IODMACommand::Create(this, …)` + `PrepareForDMA(0, bounceBuffer, 0, 0, …)` in `UserProcessParallelTask` | PrepareForDMA returns kIOReturnSuccess; bounce still all zeros |
| 6 | Address-limited HBA forces kernel bounce+fill | `UserGetDMASpecification` numAddressBits = 32 and `kIOMaximumSegmentAddressableBitCountKey` = 32 | No fill; probe stalls after TEST UNIT READY (only TUR ever dispatched). Consistent with DART/IOMMU satisfying the constraint by remapping instead of bouncing |
| 7 | Duplicate controller task IDs cause wrong-task lookup (Apple DTS diagnosis, forums thread 837320) | Runtime bitmap of active task IDs; duplicates flagged | No duplicate ever detected; IDs unique while active |
| 8 | Inconsistent HBA constraints confuse staging | maxTransferSize 16384 with exactly 1 segment x 16384 bytes (fully coherent set) | Kernel dispatches 16 KB writes accordingly; payload still all zeros |
| 9 | Payload must be read inside the documented context | Copy performed inside `UserProcessParallelTask` itself, immediately after `UserGetDataBuffer` + `GetAddressRange`; buffer never touched later | Still all zeros at capture time |

## 5b. Earlier interpretation (2026-08-30), superseded by experiment 10

The original interpretation was based on formatting tests, not all possible
write origins. With unique task IDs, coherent constraints, correct transfer direction and
the copy performed inside the documented callback context, the buffer
returned by `UserGetDataBuffer` contained zeros for those writes. Combined
with binary inspection showing the kernel-side implementation zeroes a new
buffer and copies from the original task descriptor for write tasks, the
evidence points at the kernel's source-descriptor copy producing no data
for a software-only controller on Apple Silicon macOS 26 (26.6.2/25G83).
Public reports indicate software-backed SCSI writes work on Intel macOS 26
and on Apple Silicon macOS 27, making an OS-version-specific defect the
leading explanation. Next step: Feedback Assistant + DTS incident (draft in
docs/DTS-REPORT.md).

Timing notes: the bounce was probed (a) inside `UserProcessParallelTask`,
(b) at dequeue time in the user client external method (different queue),
(c) via a second `UserGetDataBuffer` call at dequeue time. Zeros in all
three.

## 6. Non-obvious platform findings (validated fixes, already in the repo)

These cost days; they are prerequisites for anyone reproducing:

1. `QUEUENAME(...)` queues are NOT auto-created. The dext must
   `IODispatchQueue::Create` + `SetDispatchQueue` BEFORE `Start(SUPERDISPATCH)`:
   - `"AuxiliaryQueue"` on the controller (used by `UserCreateTargetForID`).
   - `"IOUserClientQueueExternalMethod"` on the user client.
   Without them everything serializes on the controller Default queue and
   any synchronous framework call deadlocks the process in an
   uninterruptible kernel RPC (unkillable without reboot).
2. `UserCreateTargetForID` blocks until the target's initial probe completes;
   the probe's I/O is served by the same daemon that called registerTarget,
   so the daemon must not block its pump while registering.
3. Userspace async completion callbacks (`IOConnectCallAsyncScalarMethod` +
   `IONotificationPortSetDispatchQueue`) are invoked with UNPACKED
   arguments: `(refcon, result, arg0, …)`, i.e. `IOAsyncCallback1`, not the
   `(refcon, result, void **args, count)` array form.
4. IOUserClient struct I/O larger than roughly 4 KB arrives as
   `structureInputDescriptor` / `structureOutputDescriptor`
   (IOMemoryDescriptor to map with `CreateMapping`), not as `OSData`.
5. The dext bundle inside the app must be NAMED after its bundle identifier
   (`com.taunais.iscsi-initiator.dext.dext`), and `CFBundleIdentifierKernel`
   of the personality must point at `com.apple.iokit.IOSCSIParallelFamily`
   (the on-demand kext that provides the kernel-side IOClass), not
   `com.apple.kpi.iokit`.
6. The current tested transfer limit is 16384 bytes with one segment.
   The earlier 4096-byte investigation clamp is no longer current.

## 7. Relevant code (repo paths)

- `Dext/iSCSIKitDext.iig` / `Dext/iSCSIKitDext.cpp`: controller; task slots;
  `UserProcessParallelTask` fetches the bounce via `UserGetDataBuffer` and
  stores `{buffer, address, length, fBufferIOVMAddr}` per task; write-path
  probes and DMA-prepare experiment live here.
- `Dext/iSCSIKitUserClient.iig` / `.cpp`: external methods
  (RegisterCallback async, RegisterTarget, DequeueTask, CompleteTask),
  queue creation, descriptor mapping for large struct I/O.
- `Daemon/Sources/iscsikitd/DextClient.swift`: IOKit client side.
- `Daemon/Sources/iscsikitd/SessionPump.swift`: asynchronous task pump and
  read-only policy, with a local diagnostic `allowWrites` opt-in.
- Current payload instrumentation is in the dext and exposed through
  `WriteProbe_*` IORegistry properties; older daemon logs used `nz` and `stage`.
- `Daemon/Sources/CISCSIKitShared/include/iSCSIKitProtocol.h`: wire structs.

## 8. Historical reproduction

These commands describe the original diagnostic build. The current daemon
rejects writes by default. Repeating the experiment requires a disposable
LUN and a diagnostic config with `allowWrites: true`, passed using
`serve --config <path>`. A write-protected rejection does not exercise the
same end-to-end failure. The standalone `Reproducer/` still needs validation
after reboot; its dext became stuck in state U during the initial attempt.

```sh
git clone https://github.com/joaquinbejar/iSCSIKit && cd iSCSIKit
brew install xcodegen libiscsi
xcodegen generate
xcodebuild -project iSCSIKit.xcodeproj -scheme iSCSIKit -allowProvisioningUpdates build
# copy the app to /Applications (ditto), launch, Install Driver, approve
cd Daemon && swift build
.build/debug/iscsikitd serve 'iscsi://user%pass@host:3260/iqn…/0'
# in another shell, once /dev/diskN appears:
diskutil eraseDisk APFS TEST diskN     # partition-map writes report GOOD
.build/debug/iscsikitd verify 'iscsi://…/0'   # LBA 0 reads back all zeros
```

Historical daemon logs printed `cdb 0x2a dir 1 len 512 payload … nz 0`
for the formatting writes. Current builds expose `WriteProbe_*` counters in
IORegistry. Capture before/after counters separately for raw writes and
formatting; do not infer formatting correctness from a raw write alone.

## 9. Earlier investigation leads

These leads predate experiment 10. The immediate priority is a controlled
comparison of raw and buffered writes using the same known pattern, offset
and length on a disposable device, followed by independent readback. Then
validate the standalone reproducer and compare OS versions. The kernel-side
mechanism remains a hypothesis until these paths are traced or Apple confirms it.

1. **Bundled task path**: `UserMapBundledParallelTaskCommandAndResponseBuffers`
   + `UserProcessBundledParallelTasks` + `UserCompleteBundledParallelTask`.
   The framework maps shared command/response rings into the dext. Unknown
   whether the DATA path behaves differently in bundled mode; this is the
   modern path Apple promotes (WWDC20 session 10210) and the per-task path
   may simply be bit-rotted for virtual controllers. This is the single
   most promising untested lead.
2. **Disassemble the kernel shim**: class
   `IOUserSCSIParallelInterfaceController` inside
   `com.apple.iokit.IOSCSIParallelFamily` 3.0.0, linked into
   `/System/Library/KernelCollections/SystemKernelExtensions.kc` (fileset,
   stripped; needs fileset-aware tooling, e.g. ipsw/kcgrep style extraction).
   Goal: find the exact condition under which `UserGetDataBuffer`'s buffer
   is filled with caller data for the out direction; the read direction
   copy-back demonstrably exists.
3. **`PerformOperation` on IODMACommand** (`kIODMACommandPerformOperationOptionRead/Write`):
   a CPU-driven copy API between a prepared DMA mapping and a local buffer.
   Experiments so far only prepared the bounce buffer; nothing was tried
   that prepares/copies against the TASK's original memory (no descriptor
   for it is exposed, which is the crux).
4. **Apple DTS incident**: the developer account includes 2 TSIs/year. A DTS
   engineer (Kevin Elliott) is actively answering SCSIControllerDriverKit
   threads on the forums, including one about a real-hardware HBA dext
   (developer.apple.com/forums/thread/807791). No public thread covers the
   virtual-controller write-data case.
5. **Alternative family**: BlockStorageDeviceDriverKit
   (`IOUserBlockStorageDevice::DoAsyncReadWrite`) also passes a raw
   `dmaAddr`, so it likely shares the same physical-address model; not
   investigated beyond the header.

## 10. Key open questions

1. Why does the tested raw-device write deliver nonzero outbound data while
   formatting writes deliver zeros to the same dext?
2. What exactly does the kernel-side `UserGetDataBuffer` implementation do
   for `FromInitiatorToTarget` tasks: under what condition does it copy the
   caller's pages into the returned IOBufferMemoryDescriptor?
3. Does bundled mode change the data plane, or only the command/response
   plane?
4. If none of the above: how do shipping products (e.g. KernSafe iSCSI
   Initiator X, if it really works on Apple Silicon) move write data?

## 11. Crash evidence (experiment 4)

```
exception: EXC_BAD_ACCESS SIGSEGV KERN_INVALID_ADDRESS at 0x10cb7f8c000
vmregioninfo: 0x10cb7f8c000 is not in any region
faulting frames:
  _platform_memmove
  iSCSIKitDext::DaemonDequeueTask(unsigned long long, IOUserClientMethodArguments*)
  iSCSIKitUserClient::ExternalMethod(...)
```

`0x10cb7f8c000` was the task's `fBufferIOVMAddr`.

## Experiment 10 (2026-09-18): the write's origin is the discriminator

Apple DTS (case 22255765) suggested the cause was that `UserGetDataBuffer`
returns an `IOMemoryDescriptor`, not an `IOBufferMemoryDescriptor`, so
`GetAddressRange` would be failing and `CreateMapping` was needed instead.

That hypothesis was tested directly. The dext now captures the return code of
`GetAddressRange`, additionally maps the same descriptor with `CreateMapping`,
counts the nonzero payload bytes reachable through **both** routes, and
publishes the totals as IORegistry properties (dext `os_log` output is not
visible on this system, the registry always is).

Measured on macOS 26.6.2 (25G83), Apple Silicon, dext build 31:

| Write origin | Tasks | Tasks with zero payload | Bytes | Nonzero bytes |
|---|---:|---:|---:|---:|
| `dd` to `/dev/rdiskN` (raw, unbuffered) | 1 | 0 | 16384 | 16112 |
| `diskutil eraseDisk APFS` | 60 | **60** | 792576 | **0** |

`GetAddressRange` returned `kIOReturnSuccess` on all 61 tasks, `CreateMapping`
also succeeded on all 61, and both routes reported equal nonzero-byte counts
every time (`WriteProbe_RouteMismatches = 0`). This counters the specific
hypothesis that `GetAddressRange` fails or hides nonzero data visible through
`CreateMapping`. The counter compares counts, not individual bytes: it does
not establish byte-identical content for the raw write or rule out every
possible API issue. With successful full-length access, a zero nonzero-byte
count does establish that the inspected region is all zeros.

What this corrects in the original report: the defect is **not** "every write
task". The measured raw-device write delivers nonzero payload; its count
alone does not prove end-to-end integrity. The tested formatting path
delivers zeros
(`diskutil eraseDisk` fails with `-69825: Wiping volume data to prevent future
accidental probing failed`, which is the first step that writes real data).
That is why every test in experiments 1-9, all driven by `diskutil`, saw
zeros, and why an isolated `dd` test appeared to work.

The observed discriminator is raw-device writing versus formatting. A
buffered-path staging defect is the working explanation, not yet a proof
that all buffered writes fail or that every raw write is correct. A raw-only
success previously led to an incorrect conclusion that the defect was gone;
future validation must include formatting and byte-for-byte readback.
