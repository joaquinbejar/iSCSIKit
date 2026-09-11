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
    func testWriteOpcodesBlocked() {
        // Every medium-modifying opcode must be classified as such.
        for op: UInt8 in [0x0A, 0x2A, 0xAA, 0x8A, 0x2E, 0xAE, 0x8E,
                          0x41, 0x93, 0x89, 0x42, 0x04] {
            XCTAssertTrue(ReadOnlyPolicy.modifiesMedium(opcode: op),
                          "opcode 0x\(String(op, radix: 16)) must be blocked")
        }
    }

    func testReadOpcodesAllowed() {
        // Read/inquiry/capacity opcodes must pass through.
        for op: UInt8 in [0x08, 0x28, 0xA8, 0x88, 0x12, 0x9E, 0x00, 0x1A, 0x35] {
            XCTAssertFalse(ReadOnlyPolicy.modifiesMedium(opcode: op),
                           "opcode 0x\(String(op, radix: 16)) must be allowed")
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
