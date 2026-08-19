import Foundation
import ISCSIKitCore

// iscsikitd — iSCSIKit daemon CLI.
//
//   iscsikitd discover <portal>
//   iscsikitd info <iscsi-url>
//   iscsikitd verify <iscsi-url>                     (raw READ(16) smoke test)
//   iscsikitd serve <iscsi-url> [<iscsi-url>...]     (needs the dext installed)
//   iscsikitd serve --config [path]                  (target list from JSON, for launchd)
//
// iscsi-url: iscsi://[user[%pass]@]host[:port]/target-iqn/lun
// CHAP credentials travel inside the URL (or LIBISCSI_CHAP_USERNAME /
// LIBISCSI_CHAP_PASSWORD environment variables).

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

let arguments = CommandLine.arguments
guard arguments.count >= 3 else {
    fail("""
    usage:
      iscsikitd discover <portal>
      iscsikitd info <iscsi-url>
      iscsikitd verify <iscsi-url>
      iscsikitd serve <iscsi-url> [<iscsi-url>...]
      iscsikitd serve --config [path]   (default: ~/Library/Application Support/iSCSIKit/targets.json)

    iscsi-url: iscsi://[user[%pass]@]host[:port]/target-iqn/lun
    mutual CHAP: LIBISCSI_CHAP_TARGET_USERNAME / LIBISCSI_CHAP_TARGET_PASSWORD
    """)
}

do {
    switch arguments[1] {
    case "discover":
        let initiator = try Initiator()
        let targets = try initiator.discoverTargets(portal: arguments[2])
        if targets.isEmpty { print("no targets") }
        for target in targets {
            print(target.name)
            for portal in target.portals { print("  portal: \(portal)") }
        }
    case "info":
        let initiator = try Initiator()
        let url = try initiator.parseURL(arguments[2])
        try initiator.connect(to: url)
        defer { initiator.disconnect() }
        let device = try initiator.inquiry(lun: url.lun)
        let capacity = try initiator.readCapacity(lun: url.lun)
        let gib = Double(capacity.bytes) / 1_073_741_824
        print("device: \(device)")
        print("capacity: \(capacity.blocks) blocks x \(capacity.blockSize) B = \(String(format: "%.1f", gib)) GiB")
    case "verify":
        // Exercises the exact raw-CDB path `serve` uses: READ(16) of LBA 0.
        let initiator = try Initiator()
        let url = try initiator.parseURL(arguments[2])
        try initiator.connect(to: url)
        defer { initiator.disconnect() }
        let capacity = try initiator.readCapacity(lun: url.lun)
        var cdb = [UInt8](repeating: 0, count: 16)
        cdb[0] = 0x88  // READ(16)
        let blocks: UInt32 = 8
        withUnsafeBytes(of: UInt32(blocks).bigEndian) { cdb.replaceSubrange(10..<14, with: $0) }
        let result = try initiator.execute(
            lun: url.lun, cdb: Data(cdb), direction: .read,
            transferLength: blocks * capacity.blockSize)
        guard result.status == 0 else {
            fail("READ(16) failed: status 0x\(String(result.status, radix: 16)), sense \(result.sense.map { String(format: "%02x", $0) }.joined())")
        }
        let zeros = result.dataIn.allSatisfy { $0 == 0 }
        print("READ(16) OK: \(result.dataIn.count) bytes from LBA 0\(zeros ? " (all zeros)" : "")")
    case "serve":
        let entries: [DaemonConfig.TargetEntry]
        if arguments[2] == "--config" {
            let path = arguments.count > 3
                ? URL(fileURLWithPath: arguments[3])
                : DaemonConfig.defaultPath
            entries = try DaemonConfig.load(from: path).targets
            guard !entries.isEmpty else { fail("no targets in \(path.path)") }
        } else {
            entries = arguments[2...].map { DaemonConfig.TargetEntry(url: $0) }
        }
        try SessionPump().run(entries: entries)
    default:
        fail("unknown command: \(arguments[1])")
    }
} catch {
    fail("error: \(error)")
}
