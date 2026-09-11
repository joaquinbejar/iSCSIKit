import XCTest
@testable import ISCSIKitCore

final class InitiatorTests: XCTestCase {
    func testContextCreation() throws {
        _ = try Initiator()
    }

    func testCapacityMath() {
        let capacity = TargetCapacity(blocks: 1024, blockSize: 512)
        XCTAssertEqual(capacity.bytes, 524_288)
    }
}

final class ReadOnlyPolicyTests: XCTestCase {
    /// A CDB of the given opcode with the rest zeroed.
    private func cdb(_ opcode: UInt8, sa: UInt8? = nil) -> [UInt8] {
        var c = [UInt8](repeating: 0, count: 16)
        c[0] = opcode
        if let sa { c[1] = sa }
        return c
    }

    func testReadOpcodesAllowed() {
        // The commands the OS needs to probe and read a disk must pass.
        for op: UInt8 in [0x00, 0x03, 0x12, 0x1A, 0x5A, 0x25,
                          0x08, 0x28, 0xA8, 0x88, 0xA0] {
            XCTAssertTrue(ReadOnlyPolicy.isAllowed(cdb: cdb(op)),
                          "read opcode 0x\(String(op, radix: 16)) must be allowed")
        }
    }

    func testDestructiveOpcodesDenied() {
        // Every medium-modifying / erase command must be denied, including
        // the ones a blocklist would have missed (SANITIZE, SERVICE ACTION
        // OUT(16)/WRITE LONG, MAINTENANCE OUT/IN).
        for op: UInt8 in [0x0A, 0x2A, 0xAA, 0x8A,   // WRITE(6/10/12/16)
                          0x2E, 0xAE, 0x8E,          // WRITE AND VERIFY
                          0x41, 0x93,                // WRITE SAME
                          0x89,                      // COMPARE AND WRITE
                          0x42,                      // UNMAP
                          0x04,                      // FORMAT UNIT
                          0x48,                      // SANITIZE
                          0x9F,                      // SERVICE ACTION OUT(16) / WRITE LONG(16)
                          0xA3, 0xA4,                // MAINTENANCE IN/OUT
                          0x15, 0x55] {              // MODE SELECT(6/10)
            XCTAssertFalse(ReadOnlyPolicy.isAllowed(cdb: cdb(op)),
                           "destructive opcode 0x\(String(op, radix: 16)) must be denied")
        }
    }

    func testServiceActionInValidated() {
        // SERVICE ACTION IN(16) 0x9E shares one opcode across reads and
        // destructive operations; only the read service actions may pass.
        XCTAssertTrue(ReadOnlyPolicy.isAllowed(cdb: cdb(0x9E, sa: 0x10)),  // READ CAPACITY(16)
                      "0x9E/0x10 READ CAPACITY(16) must be allowed")
        XCTAssertTrue(ReadOnlyPolicy.isAllowed(cdb: cdb(0x9E, sa: 0x12)),  // GET LBA STATUS
                      "0x9E/0x12 GET LBA STATUS must be allowed")
        XCTAssertFalse(ReadOnlyPolicy.isAllowed(cdb: cdb(0x9E, sa: 0x18)), // REMOVE ELEMENT AND TRUNCATE
                       "0x9E/0x18 REMOVE ELEMENT AND TRUNCATE must be denied")
        XCTAssertFalse(ReadOnlyPolicy.isAllowed(cdb: cdb(0x9E, sa: 0x19)), // RESTORE ELEMENTS AND REBUILD
                       "0x9E/0x19 RESTORE ELEMENTS AND REBUILD must be denied")
        // Every other 0x9E service action is denied too, and a truncated
        // 0x9E CDB (no service-action byte) never passes.
        for sa: UInt8 in 0...0x1F where sa != 0x10 && sa != 0x12 {
            XCTAssertFalse(ReadOnlyPolicy.isAllowed(cdb: cdb(0x9E, sa: sa)),
                           "0x9E/0x\(String(sa, radix: 16)) must be denied")
        }
        XCTAssertFalse(ReadOnlyPolicy.isAllowed(cdb: [0x9E]))
        XCTAssertFalse(ReadOnlyPolicy.isAllowed(cdb: []))
    }

    func testDefaultDeny() {
        // For plain opcodes the decision equals allowlist membership; the
        // service-action opcode is governed by its SA table, never by opcode.
        for op in 0...255 {
            let code = UInt8(op)
            let expected = ReadOnlyPolicy.allowedServiceActions[code] == nil
                && ReadOnlyPolicy.allowedOpcodes.contains(code)
            XCTAssertEqual(ReadOnlyPolicy.isAllowed(cdb: cdb(code)), expected,
                           "opcode 0x\(String(code, radix: 16)) decision must match policy")
        }
        // No destructive or service-action opcode leaked into the plain allowlist.
        for op: UInt8 in [0x0A, 0x2A, 0xAA, 0x8A, 0x41, 0x93, 0x42, 0x04, 0x48, 0x9F, 0x9E, 0xA3, 0xA4, 0x15, 0x55] {
            XCTAssertFalse(ReadOnlyPolicy.allowedOpcodes.contains(op))
        }
    }

    func testWriteProtectedSense() {
        let s = ReadOnlyPolicy.writeProtectedSense()
        XCTAssertEqual(s[0], 0x70)   // fixed format
        XCTAssertEqual(s[2] & 0x0F, 0x07)  // DATA PROTECT
        XCTAssertEqual(s[12], 0x27)  // WRITE PROTECTED
        XCTAssertEqual(s[13], 0x00)
    }
}
