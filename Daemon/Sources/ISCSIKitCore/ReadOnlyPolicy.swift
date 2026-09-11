import Foundation

/// Enforces read-only access to a target with a default-deny allowlist.
///
/// The write data path is broken on Apple Silicon macOS 26 (the kernel stages
/// zeros for outbound transfers), so any command that modifies the medium and
/// reached the target would corrupt the LUN. A blocklist is unsafe here: it is
/// impossible to enumerate every destructive opcode (SANITIZE, WRITE SAME,
/// UNMAP, FORMAT UNIT, WRITE LONG, vendor-specific, …). Instead this policy
/// permits ONLY the small set of non-destructive read/discovery commands the
/// OS needs to probe and read a disk, and rejects everything else — including
/// every unknown opcode.
public enum ReadOnlyPolicy {
    /// Non-destructive commands allowed through to the target. Every one only
    /// reads state or medium; none can modify or erase it. The write/erase
    /// counterparts live at different opcodes (e.g. SERVICE ACTION OUT(16)
    /// 0x9F, MAINTENANCE OUT 0xA4, MODE SELECT 0x15/0x55) and are therefore
    /// denied by default.
    public static let allowedOpcodes: Set<UInt8> = [
        0x00, // TEST UNIT READY
        0x03, // REQUEST SENSE
        0x12, // INQUIRY
        0x1A, // MODE SENSE(6)
        0x5A, // MODE SENSE(10)
        0x25, // READ CAPACITY(10)
        0x9E, // SERVICE ACTION IN(16): READ CAPACITY(16)/GET LBA STATUS/READ LONG(16) — all reads
        0xA3, // MAINTENANCE IN — report/read only (OUT is 0xA4)
        0x08, // READ(6)
        0x28, // READ(10)
        0xA8, // READ(12)
        0x88, // READ(16)
        0xA0, // REPORT LUNS
        0x1E, // PREVENT ALLOW MEDIUM REMOVAL (lock state, not medium)
        0x35, // SYNCHRONIZE CACHE(10) — flush only; harmless with writes blocked
        0x91, // SYNCHRONIZE CACHE(16)
        0x4D, // LOG SENSE
        0x37, // READ DEFECT DATA(10)
        0xB7, // READ DEFECT DATA(12)
        0x1C, // RECEIVE DIAGNOSTIC RESULTS
        0x3C, // READ BUFFER
        0x2F, // VERIFY(10) — compares, does not write medium
        0xAF, // VERIFY(12)
        0x8F, // VERIFY(16)
    ]

    /// True only for an explicitly-allowed, non-destructive command. Any
    /// opcode not on the allowlist (known-destructive, vendor-specific, or
    /// simply unrecognized) returns false and must be rejected.
    public static func isAllowed(opcode: UInt8) -> Bool {
        allowedOpcodes.contains(opcode)
    }

    /// Fixed-format sense for DATA PROTECT / WRITE PROTECTED (key 0x07,
    /// ASC 0x27, ASCQ 0x00): what a write-protected medium returns.
    public static func writeProtectedSense() -> [UInt8] {
        var sense = [UInt8](repeating: 0, count: 18)
        sense[0] = 0x70  // fixed format, current error
        sense[2] = 0x07  // sense key: DATA PROTECT
        sense[7] = 10    // additional sense length
        sense[12] = 0x27 // ASC: WRITE PROTECTED
        sense[13] = 0x00 // ASCQ
        return sense
    }
}
