import CLibISCSI
import Foundation

public enum ISCSIError: Error, CustomStringConvertible {
    case contextCreationFailed
    case libiscsi(String)

    public var description: String {
        switch self {
        case .contextCreationFailed: return "could not create iscsi context"
        case .libiscsi(let message): return message
        }
    }
}

public struct DiscoveredTarget: Sendable {
    public let name: String
    public let portals: [String]
}

public struct TargetCapacity: Sendable {
    public let blocks: UInt64
    public let blockSize: UInt32

    public var bytes: UInt64 { blocks * UInt64(blockSize) }
}

public struct CHAPCredentials: Sendable {
    public let username: String
    public let password: String

    public init(username: String, password: String) {
        self.username = username
        self.password = password
    }
}

/// Thin synchronous wrapper over a libiscsi context. One instance per session;
/// not thread-safe, confine each instance to a single thread or actor.
public final class Initiator {
    public static let defaultInitiatorName = "iqn.2026-08.com.taunais.iscsikit:initiator"

    private let context: OpaquePointer

    public init(initiatorName: String = Initiator.defaultInitiatorName) throws {
        guard let ctx = iscsi_create_context(initiatorName) else {
            throw ISCSIError.contextCreationFailed
        }
        self.context = ctx
    }

    deinit {
        iscsi_destroy_context(context)
    }

    private func lastError() -> ISCSIError {
        .libiscsi(iscsi_get_error(context).map { String(cString: $0) } ?? "unknown libiscsi error")
    }

    private func check(_ result: Int32) throws {
        guard result == 0 else { throw lastError() }
    }

    // MARK: - Discovery

    /// SendTargets discovery against a portal ("host" or "host:port").
    public func discoverTargets(portal: String) throws -> [DiscoveredTarget] {
        try check(iscsi_set_session_type(context, ISCSI_SESSION_DISCOVERY))
        try check(iscsi_connect_sync(context, portal))
        try check(iscsi_login_sync(context))
        defer { iscsi_disconnect(context) }

        guard let head = iscsi_discovery_sync(context) else { throw lastError() }
        defer { iscsi_free_discovery_data(context, head) }

        var targets: [DiscoveredTarget] = []
        var node: UnsafeMutablePointer<iscsi_discovery_address>? = head
        while let current = node {
            let name = current.pointee.target_name.map { String(cString: $0) } ?? ""
            var portals: [String] = []
            var portalNode = current.pointee.portals
            while let portal = portalNode {
                if let text = portal.pointee.portal { portals.append(String(cString: text)) }
                portalNode = portal.pointee.next
            }
            targets.append(DiscoveredTarget(name: name, portals: portals))
            node = current.pointee.next
        }
        return targets
    }

    // MARK: - Session

    /// Components of an `iscsi://[user[%pass]@]host[:port]/target-iqn/lun` URL.
    public struct TargetURL: Sendable {
        public let portal: String
        public let target: String
        public let lun: Int32
        public let chap: CHAPCredentials?
        /// Mutual CHAP: credentials the target must prove to us. Populated
        /// from LIBISCSI_CHAP_TARGET_USERNAME / LIBISCSI_CHAP_TARGET_PASSWORD.
        public let targetChap: CHAPCredentials?

        public var description: String { "\(target) lun \(lun) @ \(portal)" }
    }

    /// Parses a full iSCSI URL with libiscsi's own parser.
    public func parseURL(_ url: String) throws -> TargetURL {
        guard let parsed = iscsi_parse_full_url(context, url) else { throw lastError() }
        defer { iscsi_destroy_url(parsed) }
        let value = parsed.pointee
        let portal = withUnsafeBytes(of: value.portal) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
        let target = withUnsafeBytes(of: value.target) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
        let user = withUnsafeBytes(of: value.user) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
        let passwd = withUnsafeBytes(of: value.passwd) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
        let targetUser = withUnsafeBytes(of: value.target_user) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
        let targetPasswd = withUnsafeBytes(of: value.target_passwd) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
        let chap = user.isEmpty ? nil : CHAPCredentials(username: user, password: passwd)
        let targetChap = targetUser.isEmpty ? nil : CHAPCredentials(username: targetUser, password: targetPasswd)
        return TargetURL(portal: portal, target: target, lun: value.lun, chap: chap, targetChap: targetChap)
    }

    /// Full login to a target LUN. After this call succeeds, I/O methods are usable.
    public func connect(portal: String, target: String, lun: Int32,
                        chap: CHAPCredentials? = nil) throws {
        try check(iscsi_set_session_type(context, ISCSI_SESSION_NORMAL))
        try check(iscsi_set_targetname(context, target))
        if let chap {
            try check(iscsi_set_initiator_username_pwd(context, chap.username, chap.password))
        }
        try check(iscsi_full_connect_sync(context, portal, lun))
    }

    public func connect(to url: TargetURL) throws {
        if let mutual = url.targetChap {
            try check(iscsi_set_target_username_pwd(context, mutual.username, mutual.password))
        }
        try connect(portal: url.portal, target: url.target, lun: url.lun, chap: url.chap)
    }

    /// Tears down the TCP session and logs in again. Used after network
    /// errors and system wake.
    public func reconnect() throws {
        try check(iscsi_force_reconnect_sync(context))
    }

    public func disconnect() {
        iscsi_disconnect(context)
    }

    // MARK: - I/O

    public func readCapacity(lun: Int32) throws -> TargetCapacity {
        guard let task = iscsi_readcapacity16_sync(context, lun) else { throw lastError() }
        defer { scsi_free_scsi_task(task) }
        guard task.pointee.status == SCSI_STATUS_GOOD.rawValue,
              let raw = scsi_datain_unmarshall(task) else { throw lastError() }
        let capacity = raw.assumingMemoryBound(to: scsi_readcapacity16.self).pointee
        return TargetCapacity(blocks: capacity.returned_lba &+ 1, blockSize: capacity.block_length)
    }

    public func inquiry(lun: Int32) throws -> String {
        guard let task = iscsi_inquiry_sync(context, lun, 0, 0, 255) else { throw lastError() }
        defer { scsi_free_scsi_task(task) }
        guard task.pointee.status == SCSI_STATUS_GOOD.rawValue,
              let raw = scsi_datain_unmarshall(task) else { throw lastError() }
        let inquiry = raw.assumingMemoryBound(to: scsi_inquiry_standard.self).pointee
        let vendor = withUnsafeBytes(of: inquiry.vendor_identification) { buffer in
            String(decoding: buffer.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
        let product = withUnsafeBytes(of: inquiry.product_identification) { buffer in
            String(decoding: buffer.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
        return "\(vendor.trimmingCharacters(in: .whitespaces)) \(product.trimmingCharacters(in: .whitespaces))"
    }

    // MARK: - Raw CDB passthrough

    public enum TransferDirection: Sendable {
        case none, read, write

        var xferDir: Int32 {
            switch self {
            case .none: return Int32(SCSI_XFER_NONE.rawValue)
            case .read: return Int32(SCSI_XFER_READ.rawValue)
            case .write: return Int32(SCSI_XFER_WRITE.rawValue)
            }
        }
    }

    public struct RawResult: Sendable {
        public let status: UInt8
        public let dataIn: Data
        public let sense: Data
    }

    /// Executes an arbitrary CDB against the logged-in session. This is the
    /// data path for the dext: macOS builds the CDBs, we just transport them.
    public func execute(lun: Int32, cdb: Data, direction: TransferDirection,
                        transferLength: UInt32, dataOut: Data? = nil) throws -> RawResult {
        var cdbBytes = [UInt8](cdb)
        guard let task = scsi_create_task(Int32(cdbBytes.count), &cdbBytes,
                                          direction.xferDir, Int32(transferLength)) else {
            throw lastError()
        }
        defer { scsi_free_scsi_task(task) }

        var outBytes = [UInt8](dataOut ?? Data())
        let result: UnsafeMutablePointer<scsi_task>? = outBytes.withUnsafeMutableBufferPointer { buffer in
            if direction == .write, let base = buffer.baseAddress, !buffer.isEmpty {
                var payload = iscsi_data(size: buffer.count, data: base)
                return iscsi_scsi_command_sync(context, lun, task, &payload)
            }
            return iscsi_scsi_command_sync(context, lun, task, nil)
        }
        guard result != nil else { throw lastError() }

        let status = UInt8(truncatingIfNeeded: task.pointee.status)
        var dataIn = Data()
        if direction == .read, task.pointee.datain.size > 0, let base = task.pointee.datain.data {
            dataIn = Data(bytes: base, count: Int(task.pointee.datain.size))
        }

        // libiscsi parses sense data; rebuild fixed-format bytes for the host.
        var sense = Data()
        if status == UInt8(SCSI_STATUS_CHECK_CONDITION.rawValue) {
            var fixed = [UInt8](repeating: 0, count: 18)
            fixed[0] = task.pointee.sense.error_type
            fixed[2] = UInt8(truncatingIfNeeded: task.pointee.sense.key.rawValue)
            fixed[7] = 10
            fixed[12] = UInt8(truncatingIfNeeded: task.pointee.sense.ascq >> 8)
            fixed[13] = UInt8(truncatingIfNeeded: task.pointee.sense.ascq & 0xFF)
            sense = Data(fixed)
        }
        return RawResult(status: status, dataIn: dataIn, sense: sense)
    }

    public func read(lun: Int32, lba: UInt64, blocks: UInt32, blockSize: UInt32) throws -> Data {
        guard let task = iscsi_read16_sync(context, lun, lba, blocks * blockSize,
                                           Int32(blockSize), 0, 0, 0, 0, 0) else {
            throw lastError()
        }
        defer { scsi_free_scsi_task(task) }
        guard task.pointee.status == SCSI_STATUS_GOOD.rawValue else { throw lastError() }
        return Data(bytes: task.pointee.datain.data, count: Int(task.pointee.datain.size))
    }
}
