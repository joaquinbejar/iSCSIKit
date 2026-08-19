import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var extensionManager: ExtensionManager
    @StateObject private var targetStore = TargetStore()
    @StateObject private var daemon = DaemonController()

    @State private var newName = ""
    @State private var newURL = ""
    @State private var newMutualUser = ""
    @State private var newMutualPass = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("iSCSIKit")
                .font(.largeTitle.bold())
            Text("Modern iSCSI initiator for Apple Silicon Macs")
                .foregroundStyle(.secondary)

            GroupBox("Driver") {
                HStack {
                    driverStatusView
                    Spacer()
                    Button("Install Driver") { extensionManager.activate() }
                    Button("Remove Driver") { extensionManager.deactivate() }
                }
                .padding(4)
            }

            GroupBox("Targets") {
                VStack(alignment: .leading, spacing: 8) {
                    if targetStore.targets.isEmpty {
                        Text("No targets configured")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(targetStore.targets) { target in
                        HStack {
                            VStack(alignment: .leading) {
                                Text(target.name).font(.headline)
                                Text(target.url)
                                    .font(.caption.monospaced())
                                    .foregroundStyle(.secondary)
                                    .textSelection(.enabled)
                            }
                            Spacer()
                            Button(role: .destructive) {
                                targetStore.remove(target)
                            } label: {
                                Image(systemName: "trash")
                            }
                            .disabled(daemon.state != .stopped)
                        }
                    }
                    Divider()
                    HStack {
                        TextField("Name", text: $newName)
                            .frame(width: 120)
                        TextField("iscsi://user%pass@host:3260/iqn…/0", text: $newURL)
                            .font(.body.monospaced())
                    }
                    HStack {
                        TextField("Mutual CHAP user (optional)", text: $newMutualUser)
                        SecureField("Mutual CHAP password", text: $newMutualPass)
                        Button("Add") {
                            targetStore.add(name: newName, url: newURL,
                                            mutualUsername: newMutualUser,
                                            mutualPassword: newMutualPass)
                            newName = ""; newURL = ""
                            newMutualUser = ""; newMutualPass = ""
                        }
                        .disabled(newName.isEmpty || !newURL.hasPrefix("iscsi://"))
                    }
                }
                .padding(4)
            }

            GroupBox("Daemon") {
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        daemonStatusView
                        Spacer()
                        Button("Connect All") {
                            daemon.start(urls: targetStore.targets.map(\.url))
                        }
                        .disabled(daemon.state != .stopped || targetStore.targets.isEmpty)
                        Button("Disconnect") { daemon.stop() }
                            .disabled(daemon.state == .stopped)
                    }
                    HStack {
                        Text("Path:")
                        TextField("iscsikitd path", text: $daemon.daemonPath)
                            .font(.caption.monospaced())
                    }
                    HStack {
                        agentStatusView
                        Spacer()
                        Button("Install Login Agent") { daemon.installAgent() }
                            .disabled(daemon.agentStatus == .enabled)
                        Button("Remove Agent") { daemon.removeAgent() }
                            .disabled(daemon.agentStatus != .enabled)
                    }
                    if !daemon.log.isEmpty {
                        ScrollView {
                            Text(daemon.log)
                                .font(.caption.monospaced())
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .textSelection(.enabled)
                        }
                        .frame(height: 120)
                        .background(.quaternary.opacity(0.5))
                        .clipShape(RoundedRectangle(cornerRadius: 4))
                    }
                }
                .padding(4)
            }
        }
        .padding(24)
        .frame(minWidth: 560, minHeight: 480)
    }

    @ViewBuilder
    private var driverStatusView: some View {
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

    @ViewBuilder
    private var agentStatusView: some View {
        switch daemon.agentStatus {
        case .enabled:
            Label("Login agent installed", systemImage: "clock.badge.checkmark")
                .foregroundStyle(.green)
        case .requiresApproval:
            Label("Approve agent in System Settings › Login Items",
                  systemImage: "exclamationmark.triangle")
                .foregroundStyle(.orange)
        default:
            Label("No login agent", systemImage: "clock")
                .foregroundStyle(.secondary)
        }
    }

    @ViewBuilder
    private var daemonStatusView: some View {
        switch daemon.state {
        case .stopped:
            Label("Disconnected", systemImage: "circle.dashed")
        case .running(let pid):
            Label("Connected (pid \(pid))", systemImage: "checkmark.circle.fill")
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
