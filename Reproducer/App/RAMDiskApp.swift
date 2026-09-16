import SwiftUI
import SystemExtensions

// Installs the RAMDiskDext driver extension. Once the disk appears, any
// write to it (diskutil eraseDisk, dd) makes the dext log the number of
// nonzero payload bytes it received: 0 on Apple Silicon macOS 26.
@main
struct RAMDiskApp: App {
    @StateObject private var installer = Installer()
    var body: some Scene {
        WindowGroup {
            VStack(spacing: 12) {
                Text("RAMDisk reproducer for FB24799838").font(.headline)
                Text(installer.status)
                Button("Install Driver") { installer.activate() }
                Text("Then: diskutil list (a 64 MB \"RAMDisk repro\" disk appears), sudo diskutil eraseDisk JHFS+ T diskN, and\nsudo log stream --predicate 'eventMessage CONTAINS \"RAMDiskDext: WRITE\"'")
                    .font(.caption).multilineTextAlignment(.center)
            }.padding(24).frame(minWidth: 520)
        }
    }
}

final class Installer: NSObject, ObservableObject, OSSystemExtensionRequestDelegate {
    @Published var status = "Driver not installed"
    static let dextIdentifier = "com.example.ramdisk-repro.dext"

    func activate() {
        status = "Activating…"
        let request = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: Self.dextIdentifier, queue: .main)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }
    func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction { .replace }
    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        status = "Approve in System Settings › Login Items & Extensions"
    }
    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        status = "Driver active"
    }
    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        status = "Failed: \(error.localizedDescription)"
    }
}
