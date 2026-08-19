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
