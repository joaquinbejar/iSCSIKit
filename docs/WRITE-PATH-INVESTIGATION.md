# iSCSIKit: Write Data Path Investigation Report

Status: OPEN PROBLEM. Reads work end to end; write payloads never become
CPU-visible to the dext. This document is a complete, self-contained handoff
for anyone attempting to solve it.

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

For WRITE tasks (`fTransferDirection == kSCSIDataTransfer_FromInitiatorToTarget`),
the outbound payload is never observable by the dext:

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
6. Transfers are currently clamped to 4096 bytes (`kMaxTransferSize`) while
   investigating; reads worked fine at larger sizes before the clamp.

## 7. Relevant code (repo paths)

- `Dext/iSCSIKitDext.iig` / `Dext/iSCSIKitDext.cpp`: controller; task slots;
  `UserProcessParallelTask` fetches the bounce via `UserGetDataBuffer` and
  stores `{buffer, address, length, fBufferIOVMAddr}` per task; write-path
  probes and DMA-prepare experiment live here.
- `Dext/iSCSIKitUserClient.iig` / `.cpp`: external methods
  (RegisterCallback async, RegisterTarget, DequeueTask, CompleteTask),
  queue creation, descriptor mapping for large struct I/O.
- `Daemon/Sources/iscsikitd/DextClient.swift`: IOKit client side.
- `Daemon/Sources/iscsikitd/SessionPump.swift`: task pump, per-write payload
  instrumentation (`nz` = count of nonzero payload bytes, `stage` = probe
  result from `descriptor.reserved`).
- `Daemon/Sources/CISCSIKitShared/include/iSCSIKitProtocol.h`: wire structs.

## 8. Reproduction

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

The daemon prints one line per write: `cdb 0x2a dir 1 len 512 payload … nz 0`;
`nz` stays 0 for every write including GPT headers, which are provably
nonzero.

## 9. Untested avenues (ordered by expected value)

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

1. Is there ANY supported way for a software-only (virtual)
   SCSIControllerDriverKit dext on Apple Silicon to read the outbound data
   of a write task with the CPU?
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
