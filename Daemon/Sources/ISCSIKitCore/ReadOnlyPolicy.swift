import Foundation

/// Enforces read-only access to a target with a default-deny allowlist that
/// inspects the full CDB, not just the opcode.
///
/// The write data path is broken on Apple Silicon macOS 26 (the kernel stages
/// zeros for outbound transfers), so any command that modifies the medium and
/// reached the target would corrupt the LUN. A blocklist is unsafe (SANITIZE,
/// WRITE SAME, UNMAP, vendor opcodes…), and an opcode-only allowlist is also
/// unsafe: service-action opcodes share one opcode across read AND destructive
/// operations. SERVICE ACTION IN(16) 0x9E covers READ CAPACITY(16) (SA 0x10)
/// but also REMOVE ELEMENT AND TRUNCATE (SA 0x18) and RESTORE ELEMENTS AND
/// REBUILD (SA 0x19). So this policy permits only the exact (opcode, service
/// action) pairs the OS needs to probe and read a disk, and rejects everything
/// else.
public enum ReadOnlyPolicy {
    /// Single-function, non-destructive commands allowed by opcode alone.
    /// None carries a service action that changes its destructiveness.
    public static let allowedOpcodes: Set<UInt8> = [
        0x00, // TEST UNIT READY
        0x03, // REQUEST SENSE
        0x12, // INQUIRY
        0x1A, // MODE SENSE(6)
        0x5A, // MODE SENSE(10)
        0x25, // READ CAPACITY(10)
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
        0x2F, // VERIFY(10) — compares, does not write medium
        0xAF, // VERIFY(12)
        0x8F, // VERIFY(16)
    ]

    /// Service-action opcodes need the exact service action validated. Maps an
    /// opcode to the set of allowed service actions (low 5 bits of CDB byte 1).
    /// 0x9E SERVICE ACTION IN(16): only READ CAPACITY(16) 0x10 and GET LBA
    /// STATUS 0x12 — never REMOVE/RESTORE ELEMENT (0x18/0x19).
    public static let allowedServiceActions: [UInt8: Set<UInt8>] = [
        0x9E: [0x10, 0x12],
    ]

    /// Decides a full CDB. Returns true only for an explicitly-allowed,
    /// non-destructive command with a validated service action where relevant.
    public static func isAllowed(cdb: [UInt8]) -> Bool {
        guard let opcode = cdb.first else { return false }
        if let allowedSAs = allowedServiceActions[opcode] {
            guard cdb.count >= 2 else { return false }
            return allowedSAs.contains(cdb[1] & 0x1F)
        }
        return allowedOpcodes.contains(opcode)
    }

    /// Convenience for a CDB carried as a Data slice.
    public static func isAllowed(cdb: Data) -> Bool {
        isAllowed(cdb: [UInt8](cdb))
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
