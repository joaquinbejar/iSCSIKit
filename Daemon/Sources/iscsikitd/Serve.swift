import CISCSIKitShared
import Foundation
import ISCSIKitCore

/// Bridges one iSCSI session to the dext: registers the LUN as target 0 and
/// pumps SCSI tasks between the kernel and the remote target until killed.
func serve(portal: String, targetIQN: String, lun: Int32,
           chap: CHAPCredentials?) throws -> Never {
    // Single serial queue: libiscsi contexts are not thread-safe, and it also
    // orders dext callbacks.
    let queue = DispatchQueue(label: "com.taunais.iscsikit.pump")

    let initiator = try Initiator()
    try initiator.connect(portal: portal, target: targetIQN, lun: lun, chap: chap)
    let device = try initiator.inquiry(lun: lun)
    let capacity = try initiator.readCapacity(lun: lun)
    print("session up: \(device), \(capacity.blocks) x \(capacity.blockSize) B")

    let dext = try DextClient(queue: queue)
    dext.onTaskPending = { taskID in
        do {
            let (descriptor, dataOut) = try dext.dequeueTask(taskID)
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

            let result = try initiator.execute(
                lun: Int32(descriptor.lun),
                cdb: cdbData,
                direction: direction,
                transferLength: descriptor.transferLength,
                dataOut: direction == .write ? dataOut : nil
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
        } catch {
            FileHandle.standardError.write(Data("task \(taskID) failed: \(error)\n".utf8))
            var response = ISCSIKitTaskResponse()
            response.taskID = taskID
            response.status = 0x02  // CHECK CONDITION
            try? dext.completeTask(response, dataIn: Data())
        }
    }

    try queue.sync {
        try dext.registerCallback()
        try dext.registerTarget(0)
    }
    print("target 0 registered — LUN should appear as a disk")

    signal(SIGINT, SIG_IGN)
    let sigint = DispatchSource.makeSignalSource(signal: SIGINT, queue: queue)
    sigint.setEventHandler {
        print("\nshutting down")
        try? dext.unregisterTarget(0)
        initiator.disconnect()
        exit(0)
    }
    sigint.resume()

    dispatchMain()
}
