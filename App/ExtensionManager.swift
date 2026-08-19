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
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {
        log.info("finished: \(String(describing: result))")
        status = .activated
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        log.error("failed: \(error.localizedDescription)")
        status = .failed(error.localizedDescription)
    }
}
