# Draft: Feedback Assistant / DTS Incident

Title: SCSIControllerDriverKit: UserGetDataBuffer returns a zero-filled
buffer for write tasks on a software-only controller (Apple Silicon,
macOS 26)

Area: DriverKit / SCSIControllerDriverKit

## Summary

On Apple Silicon macOS 26.6.2 (25G83), a software-only (virtual)
`IOUserSCSIParallelInterfaceController` dext never receives the outbound
payload of write tasks. The `IOBufferMemoryDescriptor` returned by
`UserGetDataBuffer` is zero-filled for every
`FromInitiatorToTarget` task, while the read direction through the very
same buffer works correctly (the dext fills it; the caller receives the
data). This makes it impossible to implement a software-backed SCSI
controller (for example an iSCSI initiator) on Apple Silicon macOS 26.

## Conditions verified before filing

- `UserGetDataBuffer` is called exactly once, inside
  `UserProcessParallelTask`, and the copy is performed there, immediately
  after `GetAddressRange`.
- Controller task IDs (assigned in `UserMapHBAData`) are unique; a runtime
  bitmap asserts no ID is reused while active (addresses the duplicate-ID
  lookup issue previously diagnosed by DTS in forums thread 837320).
- `UserReportHBAConstraints` and `UserGetDMASpecification` are internally
  consistent: maxTransferSize 16384, exactly 1 segment of 16384 bytes,
  64 address bits.
- `fTransferDirection` observed at the dext is 1 (initiator to target) and
  `fRequestedTransferCount` matches the CDB.
- The zero result was confirmed with provably nonzero payloads (GPT
  headers written by `diskutil eraseDisk`) and verified out of band by
  reading the LBAs back over iSCSI directly: the medium receives zeros.

## Additional data points

- `fBufferIOVMAddr` is not mapped in the dext address space (SIGSEGV on
  access), as expected for a DMA address; a software controller has no
  DMA engine, so `UserGetDataBuffer` is its only documented data access.
- `IODMACommand::PrepareForDMA` on the returned buffer succeeds but does
  not change the observed contents.
- Reports on the developer forums indicate software-backed SCSI writes
  work on Intel macOS 26 and on Apple Silicon macOS 27 (threads 837879,
  837425), suggesting a version-specific regression on Apple Silicon
  macOS 26.

## Reproduction

Complete open-source reproducer: https://github.com/joaquinbejar/iSCSIKit
(commit <fill in>). Steps in docs/WRITE-PATH-INVESTIGATION.md section 8;
the daemon logs one line per write with a nonzero-byte count of the
payload observed by the dext, which stays 0 for every write.

## Questions

1. Under what condition does the kernel copy the caller's outbound data
   into the buffer returned by `UserGetDataBuffer`?
2. Is a software-only SCSIControllerDriverKit controller a supported
   configuration on Apple Silicon, and if so, what is the intended write
   data path?

## Attachments to include when filing

- sysdiagnose captured right after a failing `diskutil eraseDisk`
- The dext crash log from the `fBufferIOVMAddr` experiment
  (com.taunais.iscsi-initiator.dext-2026-08-30-052336.ips)
- docs/WRITE-PATH-INVESTIGATION.md
