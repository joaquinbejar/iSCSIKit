import Foundation
import SystemExtensions
import os

/// Drives activation/deactivation of the iSCSIKit driver extension.
final class ExtensionManager: NSObject, ObservableObject {
    static let dextIdentifier = "com.taunais.iscsi-initiator.dext"

    enum Status: Equatable {
        case unknown
        case activating
        case needsApproval
        case activated
        case failed(String)
    }

    @Published var status: Status = .unknown

    private let log = Logger(subsystem: "com.taunais.iscsikit", category: "ExtensionManager")
    private var propertiesProbe: OSSystemExtensionRequest?

    override init() {
        super.init()
        refreshStatus()
    }

    /// Queries the real state of the installed extension so the UI never
    /// lies at launch.
    func refreshStatus() {
        let request = OSSystemExtensionRequest.propertiesRequest(
            forExtensionWithIdentifier: Self.dextIdentifier,
            queue: .main
        )
        request.delegate = self
        propertiesProbe = request
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func activate() {
        status = .activating
        let request = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: Self.dextIdentifier,
            queue: .main
        )
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func deactivate() {
        let request = OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: Self.dextIdentifier,
            queue: .main
        )
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }
}

extension ExtensionManager: OSSystemExtensionRequestDelegate {
    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        log.info("replacing \(existing.bundleShortVersion) with \(ext.bundleShortVersion)")
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        log.info("needs user approval (System Settings > Login Items & Extensions)")
        status = .needsApproval
    }

    func request(_ request: OSSystemExtensionRequest,
                 foundProperties properties: [OSSystemExtensionProperties]) {
        propertiesProbe = nil
        if properties.contains(where: { $0.isEnabled }) {
            status = .activated
        } else if properties.contains(where: { $0.isAwaitingUserApproval }) {
            status = .needsApproval
        } else {
            status = .unknown
        }
        log.info("status probe: \(properties.count) installed version(s)")
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {
        guard request !== propertiesProbe else { return }
        log.info("finished: \(String(describing: result))")
        status = .activated
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        if request === propertiesProbe {
            // No extension installed yet — probe reports an error, not a lie.
            propertiesProbe = nil
            status = .unknown
            return
        }
        log.error("failed: \(error.localizedDescription)")
        status = .failed(error.localizedDescription)
    }
}
