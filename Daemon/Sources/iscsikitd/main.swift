import Foundation
import ISCSIKitCore

// iscsikitd — iSCSIKit daemon CLI.
//
//   iscsikitd discover <portal>
//   iscsikitd info <iscsi-url>
//   iscsikitd verify <iscsi-url>                     (raw READ(16) smoke test)
//   iscsikitd bench <iscsi-url> [--mib N] [--write --lba START]
//                                                    (sequential throughput, no dext)
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
      iscsikitd bench <iscsi-url> [--mib N] [--write --lba START]   (transport-level throughput)
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
    case "bench":
        // Sequential throughput of the iSCSI transport itself, through the
        // same execute() path `serve` uses, at queue depth 1 (which is what
        // the serial task pump gives the kernel today). Reads start at LBA 0.
        // Writes are opt-in, need an explicit start LBA and overwrite that
        // region with a pattern; a read-back of the first block proves the
        // data landed (this is the path the dext cannot exercise on macOS 26).
        var mib = 256
        var doWrite = false
        var writeLBA: UInt64? = nil
        var i = 3
        while i < arguments.count {
            switch arguments[i] {
            case "--mib": i += 1; mib = Int(arguments[i]) ?? mib
            case "--write": doWrite = true
            case "--lba": i += 1; writeLBA = UInt64(arguments[i])
            default: fail("unknown bench option: \(arguments[i])")
            }
            i += 1
        }
        if doWrite && writeLBA == nil { fail("--write requires --lba START (the region is overwritten)") }
        // A distinct initiator name so the bench session never reinstates
        // (kicks out) the daemon's session on the same target.
        let initiator = try Initiator(initiatorName: "iqn.2026-08.com.taunais.iscsikit:bench")
        let url = try initiator.parseURL(arguments[2])
        try initiator.connect(to: url)
        defer { initiator.disconnect() }
        let capacity = try initiator.readCapacity(lun: url.lun)
        let blockSize = capacity.blockSize
        func cdb16(_ opcode: UInt8, lba: UInt64, blocks: UInt32) -> Data {
            var cdb = [UInt8](repeating: 0, count: 16)
            cdb[0] = opcode
            withUnsafeBytes(of: lba.bigEndian) { cdb.replaceSubrange(2..<10, with: $0) }
            withUnsafeBytes(of: blocks.bigEndian) { cdb.replaceSubrange(10..<14, with: $0) }
            return Data(cdb)
        }
        func run(label: String, opcode: UInt8, startLBA: UInt64, ioSize: Int, pattern: Data?) throws {
            let blocks = UInt32(ioSize / Int(blockSize))
            let ops = (mib * 1_048_576) / ioSize
            var lba = startLBA
            var worst = 0.0
            let t0 = Date()
            for _ in 0..<ops {
                let t = Date()
                let r = try initiator.execute(
                    lun: url.lun, cdb: cdb16(opcode, lba: lba, blocks: blocks),
                    direction: pattern == nil ? .read : .write,
                    transferLength: UInt32(ioSize), dataOut: pattern?[...])
                guard r.status == 0 else {
                    fail("\(label) failed at LBA \(lba): status 0x\(String(r.status, radix: 16)) sense \(r.sense.map { String(format: "%02x", $0) }.joined())")
                }
                worst = max(worst, Date().timeIntervalSince(t))
                lba += UInt64(blocks)
            }
            let secs = Date().timeIntervalSince(t0)
            let mbs = Double(ops * ioSize) / 1_048_576 / secs
            print(String(format: "%@ %6dK x %5d: %7.1f MiB/s  %6.0f IOPS  avg %5.2f ms  max %5.1f ms",
                         label, ioSize / 1024, ops, mbs, Double(ops) / secs, secs / Double(ops) * 1000, worst * 1000))
        }
        let sizes = [16 * 1024, 64 * 1024, 256 * 1024, 1_048_576]
        print("target \(url.description), block \(blockSize) B, \(mib) MiB per size, queue depth 1")
        for ioSize in sizes {
            try run(label: "READ(16) ", opcode: 0x88, startLBA: 0, ioSize: ioSize, pattern: nil)
        }
        if doWrite, let start = writeLBA {
            for ioSize in sizes {
                var pattern = Data(count: ioSize)
                for j in stride(from: 0, to: ioSize, by: 4) {
                    let v = UInt32(truncatingIfNeeded: 0xA5C3_0000 &+ UInt32(j / 4))
                    withUnsafeBytes(of: v) { pattern.replaceSubrange(j..<j + 4, with: $0) }
                }
                try run(label: "WRITE(16)", opcode: 0x8A, startLBA: start, ioSize: ioSize, pattern: pattern)
                // Verify EVERY region just written, not only the first, and
                // abort the whole run on any mismatch or read failure — the
                // point is to prove the write landed, so a silent difference
                // must never pass.
                let blocks = UInt32(ioSize / Int(blockSize))
                let ops = (mib * 1_048_576) / ioSize
                var lba = start
                for _ in 0..<ops {
                    let back = try initiator.execute(
                        lun: url.lun, cdb: cdb16(0x88, lba: lba, blocks: blocks),
                        direction: .read, transferLength: UInt32(ioSize))
                    guard back.status == 0 else {
                        fail("verify read failed at LBA \(lba): status 0x\(String(back.status, radix: 16)) sense \(back.sense.map { String(format: "%02x", $0) }.joined())")
                    }
                    guard back.dataIn == pattern else {
                        fail("verify MISMATCH at LBA \(lba): \(ioSize / 1024)K region read back differs from what was written")
                    }
                    lba += UInt64(blocks)
                }
                print("  verified \(ops * ioSize / 1_048_576) MiB (\(ops) x \(ioSize / 1024)K) read back, all match")
            }
        }
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
