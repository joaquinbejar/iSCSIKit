# Feedback Assistant / DTS Incident

Filed with Apple on 2026-09-16 as **FB24799838** (Developer Technologies & SDKs › DriverKit, macOS 26.6.2 25G82). DTS code-level support request **Case-ID 22255765** (same day), with the focused reproducer in `Reproducer/`.

Current title: SCSIControllerDriverKit: formatting writes arrive zero-filled
while a raw-device write delivers payload (Apple Silicon, macOS 26)

Area: DriverKit / SCSIControllerDriverKit

## Update 2026-09-18 (after DTS reply)

The DTS hypothesis (GetAddressRange failing because the descriptor is an
IOMemoryDescriptor) was measured and does not hold: it returns success on every
task and CreateMapping yields equal nonzero-byte counts. The defect is
narrower than first reported: it depends on where the write comes from. See
experiment 10 in WRITE-PATH-INVESTIGATION.md.

| Origin | Tasks | All-zero payloads | Nonzero bytes |
|---|---:|---:|---:|
| `dd` to `/dev/rdiskN` | 1 | 0 | 16112 / 16384 |
| `diskutil eraseDisk APFS` | 60 | 60 | 0 / 792576 |

Both access APIs succeeded on all 61 tasks. `WriteProbe_RouteMismatches`
compares nonzero-byte counts, so zero mismatches does not establish
byte-for-byte equality of nonzero payloads. These measurements come from
the main dext, build 31. The standalone RAMDisk reproducer is not yet
validated: its dext became stuck in state U and needs a retry after reboot.

## Summary

On Apple Silicon macOS 26.6.2 (25G83), a software-only (virtual)
`IOUserSCSIParallelInterfaceController` dext receives zero-filled buffers
for the measured `diskutil eraseDisk` writes, while one raw-device write
delivers nonzero payload. Reads work end to end. Formatting APFS remains
blocked; raw-write success does not resolve the formatting failure. The
working hypothesis is a difference in kernel staging between raw and
buffered paths, with the exact cause still unconfirmed.

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

- Verified 2026-09-15: the same WRITE(16) CDBs with the same payloads
  succeed when issued through libiscsi directly (`iscsikitd bench --write`
  writes 256 MiB per I/O size and reads every region back: all match), and
  a 1 MiB region written that way reads back byte-for-byte identical
  through the dext block device (SHA-256 compared). Reads through the dext
  reach 47 MB/s with 8 commands in flight. Only the outbound direction
  through the dext is affected.
- The controller reports one segment of 16384 bytes because
  `SCSIUserParallelTask` carries a single `fBufferIOVMAddr`; a
  multi-segment declaration makes every request larger than one page fail
  with EIO before reaching the dext, so the tested configuration is the
  only one that works for reads.

## Reproduction

Complete open-source reproducer: https://github.com/joaquinbejar/iSCSIKit
(tag v0.1.1-preview, commit e0e30c7 or later). Steps in docs/WRITE-PATH-INVESTIGATION.md section 8;
historical daemon logs showed zero payloads during formatting. Current
diagnostic builds publish `WriteProbe_*` counters in IORegistry. The daemon
is read-only by default; reproducing end-to-end formatting requires the
diagnostic opt-in on a disposable LUN. `Reproducer/README.md` describes the
standalone RAMDisk candidate, whose execution is still unverified.

## Questions

1. What differs in the kernel's outbound-data staging between raw-device
   writes and the formatting path used by `diskutil eraseDisk`?
2. Is a software-only SCSIControllerDriverKit controller a supported
   configuration on Apple Silicon, and if so, what is the intended write
   data path?

## Attachments to include when filing

- sysdiagnose captured right after a failing `diskutil eraseDisk`
- The dext crash log from the `fBufferIOVMAddr` experiment
  (com.taunais.iscsi-initiator.dext-2026-08-30-052336.ips)
- docs/WRITE-PATH-INVESTIGATION.md
