import CISCSIKitShared
import Foundation

/// Append-only task trace in ~/Library/Logs/iSCSIKit, fsynced on every flush
/// so the evidence survives a watchdog panic and reboot. Lines are
/// `t=<uptime ns> src=<dext|daemon> ev=<name> seq=<n> task=<id> ...`; seq is
/// the dext's per-task sequence number (task IDs and slots are reused).
/// Only touched on the pump queue.
final class TraceLog {
    private let handle: FileHandle?
    let path: String
    private var pending: [String] = []

    init() {
        let dir = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Logs/iSCSIKit")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let stamp = ISO8601DateFormatter().string(from: Date())
            .replacingOccurrences(of: ":", with: "")
        let url = dir.appendingPathComponent("trace-\(stamp).log")
        path = url.path
        FileManager.default.createFile(atPath: url.path, contents: nil)
        handle = try? FileHandle(forWritingTo: url)
        if handle == nil {
            FileHandle.standardError.write(Data("trace log unavailable at \(url.path)\n".utf8))
        }
    }

    static func now() -> UInt64 { clock_gettime_nsec_np(CLOCK_UPTIME_RAW) }

    func daemon(_ event: String, seq: UInt64, task: UInt64, _ detail: String = "") {
        pending.append("t=\(Self.now()) src=daemon ev=\(event) seq=\(seq) task=\(task)"
                       + (detail.isEmpty ? "" : " " + detail))
    }

    func note(_ text: String) {
        pending.append("t=\(Self.now()) src=daemon ev=Note \(text)")
    }

    func dext(_ e: ISCSIKitEvent) {
        let name = Self.eventName(e.type)
        let code = String(UInt32(e.code), radix: 16)
        pending.append("t=\(e.timeNs) src=dext ev=\(name) seq=\(e.seq) task=\(e.taskID) code=0x\(code) value=\(e.value) extra=\(e.extra)")
    }

    /// Writes everything buffered and fsyncs.
    func flush() {
        guard !pending.isEmpty, let handle else { pending.removeAll(); return }
        let text = pending.joined(separator: "\n") + "\n"
        pending.removeAll(keepingCapacity: true)
        handle.write(Data(text.utf8))
        fsync(handle.fileDescriptor)
    }

    static func eventName(_ type: UInt8) -> String {
        switch UInt32(type) {
        case kISCSIKitEvReceived.rawValue: return "Received"
        case kISCSIKitEvBufferFailed.rawValue: return "BufferFailed"
        case kISCSIKitEvNoClient.rawValue: return "NoClient"
        case kISCSIKitEvSlotReserved.rawValue: return "SlotReserved"
        case kISCSIKitEvNoSlot.rawValue: return "NoSlot"
        case kISCSIKitEvNotifyOK.rawValue: return "NotifyOK"
        case kISCSIKitEvNotifyNoCallback.rawValue: return "NotifyNoCallback"
        case kISCSIKitEvDequeueOK.rawValue: return "DequeueOK"
        case kISCSIKitEvDequeueFailed.rawValue: return "DequeueFailed"
        case kISCSIKitEvDequeueNotFound.rawValue: return "DequeueNotFound"
        case kISCSIKitEvResponse.rawValue: return "Response"
        case kISCSIKitEvResponseNotFound.rawValue: return "ResponseNotFound"
        case kISCSIKitEvCompleted.rawValue: return "Completed"
        case kISCSIKitEvDisconnectFailed.rawValue: return "DisconnectFailed"
        default: return "Unknown\(type)"
        }
    }
}
