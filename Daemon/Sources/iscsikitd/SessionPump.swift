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

/// Drives one libiscsi session from the pump queue: watches its socket with
/// dispatch sources and lets libiscsi make progress whenever the socket is
/// readable, writable (only while it has output pending) or a second has
/// passed (command timeouts). This is what lets many commands be in flight
/// on one session without threads: every libiscsi call still happens on
/// `queue`.
final class SessionLoop {
    private let initiator: Initiator
    private let queue: DispatchQueue
    private var reader: DispatchSourceRead?
    private var writer: DispatchSourceWrite?
    private var writerActive = false
    private var timer: DispatchSourceTimer?
    /// Called on `queue` when libiscsi reports a transport failure.
    var onTransportError: ((Error) -> Void)?

    init(initiator: Initiator, queue: DispatchQueue) {
        self.initiator = initiator
        self.queue = queue
    }

    func start() {
        attach()
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 1, repeating: 1)
        timer.setEventHandler { [weak self] in self?.pump(events: 0) }
        timer.resume()
        self.timer = timer
    }

    /// After a reconnect libiscsi owns a new socket; re-arm the sources on it.
    func restart() {
        detach()
        attach()
    }

    func stop() {
        detach()
        timer?.cancel()
        timer = nil
    }

    /// Re-evaluate whether libiscsi wants POLLOUT; call after queueing work.
    func update() {
        let wantsWrite = initiator.wantedEvents & POLLOUT != 0
        if wantsWrite, !writerActive {
            writer?.resume()
            writerActive = true
        } else if !wantsWrite, writerActive {
            writer?.suspend()
            writerActive = false
        }
    }

    private func attach() {
        let fd = initiator.fileDescriptor
        guard fd >= 0 else { return }
        let reader = DispatchSource.makeReadSource(fileDescriptor: fd, queue: queue)
        reader.setEventHandler { [weak self] in self?.pump(events: POLLIN) }
        reader.resume()
        self.reader = reader
        let writer = DispatchSource.makeWriteSource(fileDescriptor: fd, queue: queue)
        writer.setEventHandler { [weak self] in self?.pump(events: POLLOUT) }
        // Created suspended; `update` resumes it only while output is pending.
        self.writer = writer
        writerActive = false
        update()
    }

    private func detach() {
        reader?.cancel()
        reader = nil
        // A suspended source must be resumed before it can be cancelled.
        if let writer {
            if !writerActive { writer.resume() }
            writer.cancel()
        }
        writer = nil
        writerActive = false
    }

    private func pump(events: Int32) {
        do {
            try initiator.service(events: events)
        } catch {
            onTransportError?(error)
            return
        }
        update()
    }
}

/// Bridges one or more iSCSI sessions to the dext and pumps SCSI tasks
/// between the kernel and the remote targets until killed.
///
/// Thread-safety: every mutable access happens on `queue` (the dext callback,
/// libiscsi socket events, power notifications and signal handling are all
/// pinned to it), which is what makes the unchecked Sendable conformance
/// sound. libiscsi contexts are not thread-safe; each session's context is
/// only ever touched on `queue`.
final class SessionPump: @unchecked Sendable {
    final class Session {
        let initiator: Initiator
        let url: Initiator.TargetURL
        let loop: SessionLoop
        var reconnecting = false

        init(initiator: Initiator, url: Initiator.TargetURL, loop: SessionLoop) {
            self.initiator = initiator
            self.url = url
            self.loop = loop
        }
    }

    private let queue = DispatchQueue(label: "com.taunais.iscsikit.pump")
    private var sessions: [UInt64: Session] = [:]

    /// Where the pump's time goes, printed every few seconds while busy so
    /// the next bottleneck is measured rather than guessed.
    private struct Stats {
        var tasks = 0
        var dequeueSeconds = 0.0
        var completeSeconds = 0.0
        var maxInFlight = 0
        var lastReport = Date()
    }
    /// Tasks recovered by the stuck-task sweep (lost notifications); never
    /// reset, so one recovery is not hidden by the next stats window.
    private var stuckRecoveries = 0
    private var stuckTimer: DispatchSourceTimer?
    /// Task trace (dext ring + daemon events), flushed to disk with fsync.
    private let trace = TraceLog()
    private var traceTimer: DispatchSourceTimer?
    private var dextEventIndex: UInt64 = 0
    private var dextEventsLost: UInt64 = 0
    private var stats = Stats()
    private var statsTimer: DispatchSourceTimer?
    private var dext: DextClient?
    private var powerNotifier: io_object_t = 0
    private var powerRootPort: io_connect_t = 0

    /// Connects every entry, registers targets 0..n-1 with the dext, and
    /// blocks pumping tasks until SIGINT.
    /// Opt-in from the config file; false keeps every write off the wire.
    private var allowWrites = false
    /// Opt-in from the config file: log every command with its LBA and result.
    private var traceTasks = false

    /// Decodes the CDB far enough to say what the command actually targets.
    /// A failing format is diagnosed by which command failed and where, not
    /// by the payload, so this prints opcode, LBA and block count for every
    /// command the kernel issues.
    private static func describe(_ cdb: Data) -> String {
        guard let opcode = cdb.first else { return "empty" }
        func be(_ offset: Int, _ width: Int) -> UInt64 {
            var value: UInt64 = 0
            for i in 0..<width where offset + i < cdb.count {
                value = (value << 8) | UInt64(cdb[cdb.startIndex + offset + i])
            }
            return value
        }
        let name: String
        var lba: UInt64?
        var blocks: UInt64?
        switch opcode {
        case 0x00: name = "TEST UNIT READY"
        case 0x03: name = "REQUEST SENSE"
        case 0x04: name = "FORMAT UNIT"
        case 0x08: name = "READ(6)";  lba = be(1, 3) & 0x1FFFFF; blocks = be(4, 1)
        case 0x0A: name = "WRITE(6)"; lba = be(1, 3) & 0x1FFFFF; blocks = be(4, 1)
        case 0x12: name = "INQUIRY"
        case 0x15: name = "MODE SELECT(6)"
        case 0x1A: name = "MODE SENSE(6)"
        case 0x1B: name = "START STOP UNIT"
        case 0x1E: name = "PREVENT ALLOW MEDIUM REMOVAL"
        case 0x25: name = "READ CAPACITY(10)"
        case 0x28: name = "READ(10)";  lba = be(2, 4); blocks = be(7, 2)
        case 0x2A: name = "WRITE(10)"; lba = be(2, 4); blocks = be(7, 2)
        case 0x2F: name = "VERIFY(10)"; lba = be(2, 4); blocks = be(7, 2)
        case 0x35: name = "SYNCHRONIZE CACHE(10)"; lba = be(2, 4); blocks = be(7, 2)
        case 0x41: name = "WRITE SAME(10)"; lba = be(2, 4); blocks = be(7, 2)
        case 0x42: name = "UNMAP"
        case 0x48: name = "SANITIZE"
        case 0x4D: name = "LOG SENSE"
        case 0x55: name = "MODE SELECT(10)"
        case 0x5A: name = "MODE SENSE(10)"
        case 0x88: name = "READ(16)";  lba = be(2, 8); blocks = be(10, 4)
        case 0x8A: name = "WRITE(16)"; lba = be(2, 8); blocks = be(10, 4)
        case 0x8F: name = "VERIFY(16)"; lba = be(2, 8); blocks = be(10, 4)
        case 0x91: name = "SYNCHRONIZE CACHE(16)"; lba = be(2, 8); blocks = be(10, 4)
        case 0x93: name = "WRITE SAME(16)"; lba = be(2, 8); blocks = be(10, 4)
        case 0x9E: name = "SERVICE ACTION IN(16)/\(String(format: "%02x", cdb.count > 1 ? cdb[cdb.startIndex + 1] & 0x1F : 0))"
        case 0xA0: name = "REPORT LUNS"
        case 0xA3: name = "MAINTENANCE IN"
        default:   name = "opcode 0x\(String(format: "%02x", opcode))"
        }
        var text = name
        if let lba { text += " lba \(lba)" }
        if let blocks { text += " blocks \(blocks)" }
        return text
    }

    func run(entries: [DaemonConfig.TargetEntry], allowWrites: Bool = false,
             traceTasks: Bool = false) throws -> Never {
        self.allowWrites = allowWrites
        self.traceTasks = traceTasks
        if allowWrites {
            print("WARNING: allowWrites is on — medium-modifying commands will reach the target")
        }
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
                let loop = SessionLoop(initiator: initiator, queue: queue)
                let session = Session(initiator: initiator, url: url, loop: loop)
                loop.onTransportError = { [weak self, weak session] error in
                    guard let self, let session else { return }
                    self.reconnect(session, targetID: targetID, reason: "\(error)")
                }
                loop.start()
                sessions[targetID] = session
                let gib = Double(capacity.bytes) / 1_073_741_824
                print("target \(targetID): \(device) — \(url.description), \(String(format: "%.1f", gib)) GiB")
            }

            let dext = try DextClient(queue: queue)
            self.dext = dext
            dext.onTaskPending = { [weak self] taskID, seq in
                self?.trace.daemon("Callback", seq: seq, task: taskID)
                self?.handleTask(taskID, seq: seq)
            }
            print("task trace: \(trace.path)")
            trace.note("daemon started")
            try dext.registerCallback()

            let timer = DispatchSource.makeTimerSource(queue: queue)
            timer.schedule(deadline: .now() + 5, repeating: 5)
            timer.setEventHandler { [weak self] in self?.reportStats() }
            timer.resume()
            statsTimer = timer

            // Safety net for lost "task pending" notifications: once a
            // second, ask the dext for tasks queued but never dequeued.
            let stuck = DispatchSource.makeTimerSource(queue: queue)
            stuck.schedule(deadline: .now() + 1, repeating: 1)
            stuck.setEventHandler { [weak self] in self?.sweepStuckTasks() }
            stuck.resume()
            stuckTimer = stuck

            // Drain the dext's event ring and persist both sides' events.
            // The ring holds ISCSIKIT_EVENT_RING events (about 7 per task),
            // so 4 Hz keeps up with several thousand tasks/s.
            let traceTimer = DispatchSource.makeTimerSource(queue: queue)
            traceTimer.schedule(deadline: .now() + .milliseconds(250),
                                repeating: .milliseconds(250))
            traceTimer.setEventHandler { [weak self] in self?.drainTrace() }
            traceTimer.resume()
            self.traceTimer = traceTimer
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

    /// Copies every new dext trace event into the trace file, then fsyncs.
    private func drainTrace() {
        guard let dext else { return }
        while true {
            let batch: (events: [ISCSIKitEvent], next: UInt64, lost: UInt64)
            do {
                batch = try dext.readEvents(from: dextEventIndex)
            } catch {
                trace.note("ReadEvents failed: \(error)")
                break
            }
            if batch.lost > 0 {
                dextEventsLost += batch.lost
                trace.note("dext ring overwrote \(batch.lost) events (total lost \(dextEventsLost))")
            }
            for event in batch.events { trace.dext(event) }
            dextEventIndex = batch.next
            if batch.events.count < Int(ISCSIKIT_EVENT_BATCH) { break }
        }
        trace.flush()
    }

    /// Recovers tasks that sit in the dext's `pending` state for over 2 s.
    /// Dequeue only succeeds for pending slots, so this cannot double-run a
    /// task the notification path already took.
    private func sweepStuckTasks() {
        guard let dext else { return }
        let stuck: [(taskID: UInt64, ageMs: UInt64, seq: UInt64)]
        do {
            stuck = try dext.listPending(minAgeMs: 2000)
        } catch {
            FileHandle.standardError.write(Data("stuck sweep failed: \(error)\n".utf8))
            return
        }
        for entry in stuck {
            stuckRecoveries += 1
            print(String(format: "STUCK pending task %llu seq %llu age %.1fs; recovering (total recoveries %d)",
                         entry.taskID, entry.seq, Double(entry.ageMs) / 1000, stuckRecoveries))
            trace.daemon("STUCK", seq: entry.seq, task: entry.taskID,
                         "ageMs=\(entry.ageMs) recoveries=\(stuckRecoveries)")
            handleTask(entry.taskID, seq: entry.seq)
        }
        if !stuck.isEmpty { trace.flush() }
    }

    /// Dequeues one task from the dext and queues it on the session without
    /// waiting for the answer, so up to the dext's slot count can be in
    /// flight on the wire at once; `finish` completes it when libiscsi calls
    /// back. `seq` is what the notification said; the descriptor's seq is
    /// authoritative once dequeued.
    private func handleTask(_ taskID: UInt64, seq notifiedSeq: UInt64) {
        guard let dext else { return }
        let dequeued: (ISCSIKitTaskDescriptor, Data)
        do {
            let t0 = Date()
            dequeued = try dext.dequeueTask(taskID)
            stats.dequeueSeconds += Date().timeIntervalSince(t0)
        } catch {
            // Never complete anything here: we hold no descriptor, and a
            // completion by task id could fail a different in-flight task
            // reusing that id. If the slot had gone in flight the dext has
            // already failed that exact task (by seq).
            trace.daemon("DequeueFailed", seq: notifiedSeq, task: taskID, "error=\"\(error)\"")
            FileHandle.standardError.write(Data("task \(taskID) seq \(notifiedSeq) dequeue failed: \(error)\n".utf8))
            return
        }
        let (descriptor, dataOut) = dequeued
        let seq = descriptor.seq
        trace.daemon("DequeueOK", seq: seq, task: taskID,
                     "notifiedSeq=\(notifiedSeq) len=\(descriptor.transferLength) dir=\(descriptor.direction)")
        do {
            stats.tasks += 1
            guard let session = sessions[descriptor.targetID] else {
                try completeFailed(taskID: taskID, targetID: descriptor.targetID, seq: seq)
                return
            }

            var cdb = descriptor.cdb
            let cdbData = withUnsafeBytes(of: &cdb) {
                Data($0.prefix(Int(descriptor.cdbLength)))
            }

            // Default-deny read-only guard: anything not on the allowlist is
            // rejected before it can reach the transport, so no destructive
            // command (WRITE, SANITIZE, WRITE SAME, UNMAP, FORMAT, WRITE LONG,
            // an unknown/vendor opcode, …) can ever touch the disk. The write
            // data path is broken on Apple Silicon macOS 26 anyway (the kernel
            // stages zeros); remove this guard only once that is fixed and
            // verified.
            if !allowWrites, !ReadOnlyPolicy.isAllowed(cdb: cdbData) {
                try completeWriteProtected(taskID: descriptor.taskID,
                                           targetID: descriptor.targetID, seq: seq)
                print("task \(taskID): cdb 0x\(String(format: "%02x", descriptor.cdb.0)) REJECTED (read-only)")
                return
            }

            if traceTasks {
                let nonzero = dataOut.reduce(0) { $1 != 0 ? $0 + 1 : $0 }
                print("task \(taskID) -> \(Self.describe(cdbData)) len \(descriptor.transferLength) dir \(descriptor.direction) payloadNonzero \(nonzero)")
            }

            let direction: Initiator.TransferDirection
            switch UInt32(descriptor.direction) {
            case kISCSIKitWrite.rawValue: direction = .write
            case kISCSIKitRead.rawValue: direction = .read
            default: direction = .none
            }

            // Address the LUN the session actually logged into, not
            // descriptor.lun: the virtual HBA always presents LUN 0, so the
            // kernel's task carries 0 while the remote LUN may be any value.
            let targetID = descriptor.targetID
            let responseTaskID = descriptor.taskID
            // dataOut.count is NOT the payload size: the dequeue output buffer
            // is out of line, and IOKit then reports the caller's full buffer
            // (1 MiB) as the output length. Reporting that as bytes written
            // made every write complete as 1 MiB, so pwrite() returned more
            // than requested (dd "short write", wipefs err=45, -69825).
            let outCount = min(dataOut.count, Int(descriptor.transferLength))
            try session.initiator.executeAsync(
                lun: session.url.lun,
                cdb: cdbData,
                direction: direction,
                transferLength: descriptor.transferLength,
                dataOut: direction == .write ? dataOut.prefix(Int(descriptor.transferLength)) : nil
            ) { [weak self] outcome in
                self?.finish(taskID: responseTaskID, seq: seq, targetID: targetID,
                             direction: direction, bytesOut: outCount, outcome: outcome)
            }
            // libiscsi now has output pending; make sure POLLOUT is watched.
            session.loop.update()
            stats.maxInFlight = max(stats.maxInFlight, session.initiator.inFlight)
        } catch {
            FileHandle.standardError.write(Data("task \(taskID) failed: \(error)\n".utf8))
            try? completeFailed(taskID: taskID, targetID: descriptor.targetID, seq: seq)
        }
    }

    /// Completion for a queued command; runs on `queue` from the session's
    /// socket service.
    private func finish(taskID: UInt64, seq: UInt64, targetID: UInt64,
                        direction: Initiator.TransferDirection,
                        bytesOut: Int, outcome: Result<Initiator.RawResult, Error>) {
        guard let dext else { return }
        switch outcome {
        case .success(let result):
            trace.daemon("Response", seq: seq, task: taskID,
                         "status=0x\(String(result.status, radix: 16)) residual=\(result.residual) dataIn=\(result.dataIn.count)")
            if traceTasks, result.status != 0 {
                print("task \(taskID) <- status 0x\(String(format: "%02x", result.status)) sense \(result.sense.map { String(format: "%02x", $0) }.joined())")
            } else if traceTasks {
                print("task \(taskID) <- GOOD in \(result.dataIn.count)B")
            }
            var response = ISCSIKitTaskResponse()
            response.taskID = taskID
            response.targetID = targetID
            response.status = result.status
            // Bytes sent are not bytes written: a write counts only what the
            // target accepted, i.e. nothing unless GOOD, minus its residual.
            response.bytesTransferred = direction == .write
                ? (result.status == 0 ? UInt64(max(0, bytesOut - result.residual)) : 0)
                : UInt64(result.dataIn.count)
            withUnsafeMutableBytes(of: &response.sense) { senseBuffer in
                let count = min(result.sense.count, senseBuffer.count)
                result.sense.copyBytes(to: senseBuffer, count: count)
                response.senseLength = UInt8(count)
            }
            let t0 = Date()
            if (try? sendCompletion(response, dataIn: result.dataIn, seq: seq)) != nil {
                stats.completeSeconds += Date().timeIntervalSince(t0)
            }
        case .failure(let error):
            // A transport failure (cancelled, timed out, connection lost) is
            // reported to the kernel as a failed task, which it retries; the
            // session itself is reconnected once, not once per task.
            FileHandle.standardError.write(Data("task \(taskID) transport error: \(error)\n".utf8))
            trace.daemon("TransportError", seq: seq, task: taskID, "error=\"\(error)\"")
            try? completeFailed(taskID: taskID, targetID: targetID, seq: seq)
            if let session = sessions[targetID] {
                reconnect(session, targetID: targetID, reason: "\(error)")
            }
        }
    }

    /// Transparent reconnect. iSCSI is designed for this: the target replays
    /// nothing, in-flight commands come back cancelled (and are failed to the
    /// kernel, which resubmits them), and the socket is re-armed.
    private func reconnect(_ session: Session, targetID: UInt64, reason: String) {
        guard !session.reconnecting else { return }
        session.reconnecting = true
        defer { session.reconnecting = false }
        FileHandle.standardError.write(Data("target \(targetID): \(reason); reconnecting\n".utf8))
        do {
            try session.initiator.reconnect()
            session.loop.restart()
        } catch {
            FileHandle.standardError.write(
                Data("target \(targetID) reconnect failed: \(error)\n".utf8))
        }
    }

    private func reportStats() {
        guard stats.tasks > 0 else { return }
        let seconds = Date().timeIntervalSince(stats.lastReport)
        let perTaskDequeue = stats.dequeueSeconds / Double(stats.tasks) * 1000
        let perTaskComplete = stats.completeSeconds / Double(stats.tasks) * 1000
        print(String(format: "pump: %d tasks in %.1fs (%.0f/s), dequeue %.2f ms + complete %.2f ms per task, max in flight %d, stuck recoveries %d",
                     stats.tasks, seconds, Double(stats.tasks) / seconds,
                     perTaskDequeue, perTaskComplete, stats.maxInFlight, stuckRecoveries))
        stats = Stats()
    }

    /// completeTask plus its return code in the trace.
    private func sendCompletion(_ response: ISCSIKitTaskResponse, dataIn: Data, seq: UInt64) throws {
        guard let dext else { return }
        do {
            try dext.completeTask(response, dataIn: dataIn)
            trace.daemon("CompleteRC", seq: seq, task: response.taskID,
                         "rc=0x0 status=0x\(String(response.status, radix: 16)) bytes=\(response.bytesTransferred)")
        } catch {
            var rc: kern_return_t = KERN_FAILURE
            if case DextClientError.ioKit(_, let code) = error { rc = code }
            trace.daemon("CompleteRC", seq: seq, task: response.taskID,
                         "rc=0x\(String(UInt32(bitPattern: rc), radix: 16))")
            FileHandle.standardError.write(Data("task \(response.taskID) seq \(seq) complete failed: \(error)\n".utf8))
            throw error
        }
    }

    private func completeFailed(taskID: UInt64, targetID: UInt64, seq: UInt64) throws {
        var response = ISCSIKitTaskResponse()
        response.taskID = taskID
        response.targetID = targetID
        response.status = 0x02  // CHECK CONDITION
        try sendCompletion(response, dataIn: Data(), seq: seq)
    }

    /// Completes a task with DATA PROTECT / WRITE PROTECTED sense so the OS
    /// treats the write as rejected by a read-only medium.
    private func completeWriteProtected(taskID: UInt64, targetID: UInt64, seq: UInt64) throws {
        var response = ISCSIKitTaskResponse()
        response.taskID = taskID
        response.targetID = targetID
        response.status = 0x02  // CHECK CONDITION
        let sense = ReadOnlyPolicy.writeProtectedSense()
        withUnsafeMutableBytes(of: &response.sense) { buffer in
            let count = min(sense.count, buffer.count)
            sense.withUnsafeBytes { buffer.copyMemory(from: UnsafeRawBufferPointer(rebasing: $0.prefix(count))) }
            response.senseLength = UInt8(count)
        }
        try sendCompletion(response, dataIn: Data(), seq: seq)
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
                reconnect(session, targetID: targetID, reason: "system woke")
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
            drainTrace()
            for targetID in sessions.keys.sorted().reversed() {
                try? dext?.unregisterTarget(targetID)
                sessions[targetID]?.loop.stop()
                sessions[targetID]?.initiator.disconnect()
            }
            exit(0)
        }
        sigint.resume()
        // Keep the source alive for the process lifetime.
        _ = Unmanaged.passRetained(sigint)
    }
}
