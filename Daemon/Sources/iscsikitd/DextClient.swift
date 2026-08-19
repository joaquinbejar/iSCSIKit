import CISCSIKitShared
import Foundation
import IOKit

enum DextClientError: Error, CustomStringConvertible {
    case serviceNotFound
    case ioKit(String, kern_return_t)

    var description: String {
        switch self {
        case .serviceNotFound:
            return "iSCSIKitDext service not found — is the driver extension installed and approved?"
        case .ioKit(let call, let code):
            return "\(call) failed: 0x\(String(UInt32(bitPattern: code), radix: 16))"
        }
    }
}

/// IOKit connection to the dext's user client. All calls must happen on one
/// thread/queue; the async "task pending" callback fires on the notification
/// port's dispatch queue.
final class DextClient {
    private var connection: io_connect_t = 0
    private var notifyPort: IONotificationPortRef?
    private let queue: DispatchQueue
    var onTaskPending: ((UInt64) -> Void)?

    init(queue: DispatchQueue) throws {
        self.queue = queue
        let service = IOServiceGetMatchingService(
            kIOMainPortDefault,
            IOServiceNameMatching(ISCSIKIT_SERVICE_NAME)
        )
        guard service != 0 else { throw DextClientError.serviceNotFound }
        defer { IOObjectRelease(service) }

        let ret = IOServiceOpen(service, mach_task_self_, 0, &connection)
        guard ret == KERN_SUCCESS else { throw DextClientError.ioKit("IOServiceOpen", ret) }
    }

    deinit {
        if let notifyPort { IONotificationPortDestroy(notifyPort) }
        if connection != 0 { IOServiceClose(connection) }
    }

    func registerCallback() throws {
        guard let port = IONotificationPortCreate(kIOMainPortDefault) else {
            throw DextClientError.ioKit("IONotificationPortCreate", KERN_FAILURE)
        }
        notifyPort = port
        IONotificationPortSetDispatchQueue(port, queue)

        let callback: IOAsyncCallback = { refcon, result, args, numArgs in
            guard result == KERN_SUCCESS, numArgs >= 1, let args, let refcon else { return }
            let client = Unmanaged<DextClient>.fromOpaque(refcon).takeUnretainedValue()
            let taskID = UInt64(UInt(bitPattern: args.pointee))
            client.onTaskPending?(taskID)
        }

        var asyncRef = [UInt64](repeating: 0, count: Int(kIOAsyncCalloutCount))
        asyncRef[Int(kIOAsyncCalloutFuncIndex)] = UInt64(UInt(bitPattern: unsafeBitCast(callback, to: Int.self)))
        asyncRef[Int(kIOAsyncCalloutRefconIndex)] = UInt64(UInt(bitPattern: Int(bitPattern: Unmanaged.passUnretained(self).toOpaque())))

        var outputCount: UInt32 = 0
        let ret = IOConnectCallAsyncScalarMethod(
            connection,
            UInt32(kISCSIKitMethodRegisterCallback.rawValue),
            IONotificationPortGetMachPort(notifyPort),
            &asyncRef,
            UInt32(kIOAsyncCalloutCount),
            nil, 0,
            nil, &outputCount
        )
        guard ret == KERN_SUCCESS else {
            throw DextClientError.ioKit("RegisterCallback", ret)
        }
    }

    func registerTarget(_ targetID: UInt64) throws {
        var input = [targetID]
        let ret = IOConnectCallScalarMethod(
            connection, UInt32(kISCSIKitMethodRegisterTarget.rawValue),
            &input, 1, nil, nil
        )
        guard ret == KERN_SUCCESS else { throw DextClientError.ioKit("RegisterTarget", ret) }
    }

    func unregisterTarget(_ targetID: UInt64) throws {
        var input = [targetID]
        let ret = IOConnectCallScalarMethod(
            connection, UInt32(kISCSIKitMethodUnregisterTarget.rawValue),
            &input, 1, nil, nil
        )
        guard ret == KERN_SUCCESS else { throw DextClientError.ioKit("UnregisterTarget", ret) }
    }

    /// Returns descriptor plus dataOut payload for writes.
    func dequeueTask(_ taskID: UInt64) throws -> (ISCSIKitTaskDescriptor, Data) {
        var input = [taskID]
        let capacity = MemoryLayout<ISCSIKitTaskDescriptor>.size + Int(ISCSIKIT_MAX_TRANSFER)
        var buffer = [UInt8](repeating: 0, count: capacity)
        var length = capacity

        let ret = IOConnectCallMethod(
            connection, UInt32(kISCSIKitMethodDequeueTask.rawValue),
            &input, 1,
            nil, 0,
            nil, nil,
            &buffer, &length
        )
        guard ret == KERN_SUCCESS else { throw DextClientError.ioKit("DequeueTask", ret) }
        guard length >= MemoryLayout<ISCSIKitTaskDescriptor>.size else {
            throw DextClientError.ioKit("DequeueTask short read", KERN_FAILURE)
        }

        let descriptor = buffer.withUnsafeBytes {
            $0.load(as: ISCSIKitTaskDescriptor.self)
        }
        let payload = Data(buffer[MemoryLayout<ISCSIKitTaskDescriptor>.size..<length])
        return (descriptor, payload)
    }

    func completeTask(_ response: ISCSIKitTaskResponse, dataIn: Data) throws {
        var response = response
        var packet = Data(bytes: &response, count: MemoryLayout<ISCSIKitTaskResponse>.size)
        packet.append(dataIn)
        let ret = packet.withUnsafeBytes { bytes in
            IOConnectCallStructMethod(
                connection, UInt32(kISCSIKitMethodCompleteTask.rawValue),
                bytes.baseAddress, bytes.count,
                nil, nil
            )
        }
        guard ret == KERN_SUCCESS else { throw DextClientError.ioKit("CompleteTask", ret) }
    }
}
