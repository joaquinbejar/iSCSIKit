import CISCSIKitShared
import Foundation
import IOKit
import IOKit.pwr_mgt
import ISCSIKitCore

// IOKit power messages are C macros, invisible to Swift. Values from
// IOKit/IOMessage.h: iokit_common_msg(0x270|0x280|0x300).
private let kMessageCanSystemSleep: natural_t = 0xE000_0270
private let kMessageSystemWillSleep: natural_t = 0xE000_0280
private let kMessageSystemHasPoweredOn: natural_t = 0xE000_0300

/// Bridges one or more iSCSI sessions to the dext and pumps SCSI tasks
/// between the kernel and the remote targets until killed.
///
/// Thread-safety: every mutable access happens on `queue` (the dext callback,
/// power notifications, and signal handling are all pinned to it), which is
/// what makes the unchecked Sendable conformance sound. libiscsi contexts are
/// not thread-safe; each session's context is only ever touched on `queue`.
final class SessionPump: @unchecked Sendable {
    struct Session {
        let initiator: Initiator
        let url: Initiator.TargetURL
    }

    private let queue = DispatchQueue(label: "com.taunais.iscsikit.pump")
    private var sessions: [UInt64: Session] = [:]
    private var dext: DextClient?
    private var powerNotifier: io_object_t = 0
    private var powerRootPort: io_connect_t = 0

    /// Connects every entry, registers targets 0..n-1 with the dext, and
    /// blocks pumping tasks until SIGINT.
    func run(entries: [DaemonConfig.TargetEntry]) throws -> Never {
        try queue.sync {
            for (index, entry) in entries.enumerated() {
                let targetID = UInt64(index)
                let initiator = try Initiator()
                var url = try initiator.parseURL(entry.url)
                if let user = entry.mutualUsername, let pass = entry.mutualPassword {
                    url = url.withMutualCHAP(CHAPCredentials(username: user, password: pass))
                }
                try initiator.connect(to: url)
                let device = try initiator.inquiry(lun: url.lun)
                let capacity = try initiator.readCapacity(lun: url.lun)
                sessions[targetID] = Session(initiator: initiator, url: url)
                let gib = Double(capacity.bytes) / 1_073_741_824
                print("target \(targetID): \(device) — \(url.description), \(String(format: "%.1f", gib)) GiB")
            }

            let dext = try DextClient(queue: queue)
            self.dext = dext
            dext.onTaskPending = { [weak self] taskID in
                self?.handleTask(taskID)
            }
            try dext.registerCallback()
        }

        // Register targets OFF the pump queue: UserCreateTargetForID blocks in
        // the kernel until the target's initial probe (INQUIRY…) completes,
        // and those probe CDBs are served by the pump. Registering from the
        // pump queue deadlocks the whole stack.
        guard let dext else { throw DextClientError.serviceNotFound }
        for targetID in sessions.keys.sorted() {
            try dext.registerTarget(targetID)
        }
        print("\(sessions.count) target(s) registered — LUNs should appear as disks")

        registerForPowerNotifications()
        installSignalHandler()
        dispatchMain()
    }

    // MARK: - Task pump (always on `queue`)

    private func handleTask(_ taskID: UInt64) {
        guard let dext else { return }
        do {
            let (descriptor, dataOut) = try dext.dequeueTask(taskID)
            let nonzero = dataOut.reduce(0) { $1 != 0 ? $0 + 1 : $0 }
            let stageOffset = withUnsafeBytes(of: descriptor.reserved) { $0.load(as: UInt16.self) }
            print("task \(taskID): cdb 0x\(String(format: "%02x", descriptor.cdb.0)) dir \(descriptor.direction) len \(descriptor.transferLength) payload \(dataOut.count)B nz \(nonzero) stage 0x\(String(stageOffset, radix: 16))")
            guard let session = sessions[descriptor.targetID] else {
                try completeFailed(taskID: taskID, targetID: descriptor.targetID)
                return
            }

            var cdb = descriptor.cdb
            let cdbData = withUnsafeBytes(of: &cdb) {
                Data($0.prefix(Int(descriptor.cdbLength)))
            }
            let direction: Initiator.TransferDirection
            switch UInt32(descriptor.direction) {
            case kISCSIKitWrite.rawValue: direction = .write
            case kISCSIKitRead.rawValue: direction = .read
            default: direction = .none
            }

            let result = try executeWithReconnect(
                session: session,
                lun: Int32(descriptor.lun),
                cdb: cdbData,
                direction: direction,
                transferLength: descriptor.transferLength,
                dataOut: direction == .write
                    ? dataOut.prefix(Int(descriptor.transferLength))
                    : nil
            )

            var response = ISCSIKitTaskResponse()
            response.taskID = descriptor.taskID
            response.targetID = descriptor.targetID
            response.status = result.status
            response.bytesTransferred = direction == .write
                ? UInt64(dataOut.count)
                : UInt64(result.dataIn.count)
            withUnsafeMutableBytes(of: &response.sense) { senseBuffer in
                let count = min(result.sense.count, senseBuffer.count)
                result.sense.copyBytes(to: senseBuffer, count: count)
                response.senseLength = UInt8(count)
            }
            try dext.completeTask(response, dataIn: result.dataIn)
            print("task \(taskID): status 0x\(String(format: "%02x", result.status)) in \(result.dataIn.count)B")
        } catch {
            FileHandle.standardError.write(Data("task \(taskID) failed: \(error)\n".utf8))
            try? completeFailed(taskID: taskID, targetID: 0)
        }
    }

    /// One transparent reconnect + retry on transport failure. iSCSI is
    /// designed for this: the target replays nothing, we resubmit the CDB.
    private func executeWithReconnect(session: Session, lun: Int32, cdb: Data,
                                      direction: Initiator.TransferDirection,
                                      transferLength: UInt32,
                                      dataOut: Data?) throws -> Initiator.RawResult {
        do {
            return try session.initiator.execute(
                lun: lun, cdb: cdb, direction: direction,
                transferLength: transferLength, dataOut: dataOut)
        } catch {
            FileHandle.standardError.write(Data("transport error (\(error)); reconnecting\n".utf8))
            try session.initiator.reconnect()
            return try session.initiator.execute(
                lun: lun, cdb: cdb, direction: direction,
                transferLength: transferLength, dataOut: dataOut)
        }
    }

    private func completeFailed(taskID: UInt64, targetID: UInt64) throws {
        guard let dext else { return }
        var response = ISCSIKitTaskResponse()
        response.taskID = taskID
        response.targetID = targetID
        response.status = 0x02  // CHECK CONDITION
        try dext.completeTask(response, dataIn: Data())
    }

    // MARK: - Sleep / wake

    private func registerForPowerNotifications() {
        var notifyPort: IONotificationPortRef?
        let refcon = Unmanaged.passUnretained(self).toOpaque()
        let callback: IOServiceInterestCallback = { refcon, _, messageType, messageArgument in
            guard let refcon else { return }
            let pump = Unmanaged<SessionPump>.fromOpaque(refcon).takeUnretainedValue()
            pump.handlePowerMessage(messageType, argument: messageArgument)
        }
        powerRootPort = IORegisterForSystemPower(refcon, &notifyPort, callback, &powerNotifier)
        guard powerRootPort != 0, let notifyPort else {
            FileHandle.standardError.write(Data("power notifications unavailable\n".utf8))
            return
        }
        IONotificationPortSetDispatchQueue(notifyPort, queue)
    }

    private func handlePowerMessage(_ messageType: natural_t, argument: UnsafeMutableRawPointer?) {
        switch messageType {
        case kMessageCanSystemSleep, kMessageSystemWillSleep:
            // Never veto sleep; acknowledge immediately or the system stalls.
            IOAllowPowerChange(powerRootPort, Int(bitPattern: argument))
        case kMessageSystemHasPoweredOn:
            print("system woke — reconnecting \(sessions.count) session(s)")
            for (targetID, session) in sessions {
                do {
                    try session.initiator.reconnect()
                } catch {
                    FileHandle.standardError.write(
                        Data("target \(targetID) reconnect failed: \(error)\n".utf8))
                }
            }
        default:
            break
        }
    }

    // MARK: - Shutdown

    private func installSignalHandler() {
        signal(SIGINT, SIG_IGN)
        let sigint = DispatchSource.makeSignalSource(signal: SIGINT, queue: queue)
        sigint.setEventHandler { [self] in
            print("\nshutting down")
            for targetID in sessions.keys.sorted().reversed() {
                try? dext?.unregisterTarget(targetID)
                sessions[targetID]?.initiator.disconnect()
            }
            exit(0)
        }
        sigint.resume()
        // Keep the source alive for the process lifetime.
        _ = Unmanaged.passRetained(sigint)
    }
}
