import Foundation

/// Enforces read-only access to a target. The write data path is broken on
/// Apple Silicon macOS 26 (the kernel stages zeros for outbound transfers),
/// so any medium-modifying command that reached the target would corrupt the
/// LUN. This policy classifies such commands and produces the sense a
/// read-only medium returns, so they are rejected before leaving the host.
public enum ReadOnlyPolicy {
    /// SCSI opcodes that modify the medium: WRITE(6/10/12/16), WRITE AND
    /// VERIFY, WRITE SAME(10/16), COMPARE AND WRITE, UNMAP, FORMAT UNIT,
    /// ERASE(6/10), WRITE LONG(10/16).
    public static let mediumModifyingOpcodes: Set<UInt8> = [
        0x0A, 0x2A, 0xAA, 0x8A,
        0x2E, 0xAE, 0x8E,
        0x41, 0x93,
        0x89,
        0x42,
        0x04,
        0x19, 0x2C,
        0x3F, 0xEA,
    ]

    public static func modifiesMedium(opcode: UInt8) -> Bool {
        mediumModifyingOpcodes.contains(opcode)
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
