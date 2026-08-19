import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var extensionManager: ExtensionManager

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("iSCSIKit")
                .font(.largeTitle.bold())
            Text("Modern iSCSI initiator for Apple Silicon Macs")
                .foregroundStyle(.secondary)

            Divider()

            HStack {
                statusView
                Spacer()
                Button("Install Driver") { extensionManager.activate() }
                Button("Remove Driver") { extensionManager.deactivate() }
            }
        }
        .padding(24)
        .frame(minWidth: 480)
    }

    @ViewBuilder
    private var statusView: some View {
        switch extensionManager.status {
        case .unknown:
            Label("Driver not installed", systemImage: "circle.dashed")
        case .activating:
            Label("Activating…", systemImage: "hourglass")
        case .needsApproval:
            Label("Approve in System Settings › Login Items & Extensions",
                  systemImage: "exclamationmark.triangle")
                .foregroundStyle(.orange)
        case .activated:
            Label("Driver active", systemImage: "checkmark.circle.fill")
                .foregroundStyle(.green)
        case .failed(let message):
            Label(message, systemImage: "xmark.circle.fill")
                .foregroundStyle(.red)
        }
    }
}

#Preview {
    ContentView().environmentObject(ExtensionManager())
}
