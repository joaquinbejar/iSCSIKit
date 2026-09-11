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
    func testReadOpcodesAllowed() {
        // The commands the OS needs to probe and read a disk must pass.
        for op: UInt8 in [0x00, 0x03, 0x12, 0x1A, 0x5A, 0x25, 0x9E,
                          0x08, 0x28, 0xA8, 0x88, 0xA0] {
            XCTAssertTrue(ReadOnlyPolicy.isAllowed(opcode: op),
                          "read opcode 0x\(String(op, radix: 16)) must be allowed")
        }
    }

    func testDestructiveOpcodesDenied() {
        // Every medium-modifying / erase command must be denied, including
        // the ones a blocklist would have missed (SANITIZE, SERVICE ACTION
        // OUT(16)/WRITE LONG, MAINTENANCE OUT).
        for op: UInt8 in [0x0A, 0x2A, 0xAA, 0x8A,   // WRITE(6/10/12/16)
                          0x2E, 0xAE, 0x8E,          // WRITE AND VERIFY
                          0x41, 0x93,                // WRITE SAME
                          0x89,                      // COMPARE AND WRITE
                          0x42,                      // UNMAP
                          0x04,                      // FORMAT UNIT
                          0x48,                      // SANITIZE
                          0x9F,                      // SERVICE ACTION OUT(16) / WRITE LONG(16)
                          0xA4,                      // MAINTENANCE OUT
                          0x15, 0x55] {              // MODE SELECT(6/10)
            XCTAssertFalse(ReadOnlyPolicy.isAllowed(opcode: op),
                           "destructive opcode 0x\(String(op, radix: 16)) must be denied")
        }
    }

    func testDefaultDeny() {
        // Every opcode not on the allowlist is denied — no gaps.
        for op in 0...255 {
            let code = UInt8(op)
            XCTAssertEqual(ReadOnlyPolicy.isAllowed(opcode: code),
                           ReadOnlyPolicy.allowedOpcodes.contains(code),
                           "opcode 0x\(String(code, radix: 16)) decision must equal allowlist membership")
        }
        // No destructive opcode leaked into the allowlist.
        for op: UInt8 in [0x0A, 0x2A, 0xAA, 0x8A, 0x41, 0x93, 0x42, 0x04, 0x48, 0x9F, 0xA4, 0x15, 0x55] {
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
