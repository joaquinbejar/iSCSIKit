import Foundation
import ISCSIKitCore

// Minimal CLI while the daemon grows up:
//   iscsikitd discover <portal>
//   iscsikitd info <portal> <target-iqn> [lun]
//   iscsikitd serve <portal> <target-iqn> [lun]   (needs the dext installed)

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

let arguments = CommandLine.arguments
guard arguments.count >= 3 else {
    fail("""
    usage:
      iscsikitd discover <portal>
      iscsikitd info <portal> <target-iqn> [lun]
      iscsikitd serve <portal> <target-iqn> [lun]
    """)
}

do {
    let initiator = try Initiator()
    switch arguments[1] {
    case "discover":
        let targets = try initiator.discoverTargets(portal: arguments[2])
        if targets.isEmpty { print("no targets") }
        for target in targets {
            print(target.name)
            for portal in target.portals { print("  portal: \(portal)") }
        }
    case "info":
        guard arguments.count >= 4 else { fail("info needs <portal> <target-iqn> [lun]") }
        let lun = Int32(arguments.count > 4 ? arguments[4] : "0") ?? 0
        try initiator.connect(portal: arguments[2], target: arguments[3], lun: lun)
        defer { initiator.disconnect() }
        let device = try initiator.inquiry(lun: lun)
        let capacity = try initiator.readCapacity(lun: lun)
        let gib = Double(capacity.bytes) / 1_073_741_824
        print("device: \(device)")
        print("capacity: \(capacity.blocks) blocks x \(capacity.blockSize) B = \(String(format: "%.1f", gib)) GiB")
    case "serve":
        guard arguments.count >= 4 else { fail("serve needs <portal> <target-iqn> [lun]") }
        let lun = Int32(arguments.count > 4 ? arguments[4] : "0") ?? 0
        try serve(portal: arguments[2], targetIQN: arguments[3], lun: lun, chap: nil)
    default:
        fail("unknown command: \(arguments[1])")
    }
} catch {
    fail("error: \(error)")
}
