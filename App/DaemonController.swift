import Foundation
import ServiceManagement
import os

/// Launches and supervises one `iscsikitd serve` process for the configured
/// targets. The daemon path is user-configurable because during development
/// the binary lives in the SwiftPM build tree.
@MainActor
final class DaemonController: ObservableObject {
    enum State: Equatable {
        case stopped
        case running(pid: Int32)
        case failed(String)
    }

    @Published var state: State = .stopped
    @Published var log: String = ""

    @Published var daemonPath: String {
        didSet { UserDefaults.standard.set(daemonPath, forKey: "daemonPath") }
    }

    private var process: Process?
    private let logger = Logger(subsystem: "com.taunais.iscsikit", category: "DaemonController")

    init() {
        let bundled = Bundle.main.bundleURL
            .appendingPathComponent("Contents/MacOS/iscsikitd").path
        let fallback = FileManager.default.isExecutableFile(atPath: bundled)
            ? bundled
            : "/opt/homebrew/bin/iscsikitd"
        daemonPath = UserDefaults.standard.string(forKey: "daemonPath") ?? fallback
    }

    func start(urls: [String]) {
        guard case .stopped = state, !urls.isEmpty else { return }
        guard FileManager.default.isExecutableFile(atPath: daemonPath) else {
            state = .failed("daemon not found at \(daemonPath)")
            return
        }

        let process = Process()
        process.executableURL = URL(fileURLWithPath: daemonPath)
        process.arguments = ["serve"] + urls

        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        pipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
            Task { @MainActor [weak self] in
                self?.log.append(text)
                if let log = self?.log, log.count > 20_000 {
                    self?.log = String(log.suffix(10_000))
                }
            }
        }
        process.terminationHandler = { [weak self] finished in
            Task { @MainActor [weak self] in
                self?.process = nil
                if finished.terminationStatus == 0 {
                    self?.state = .stopped
                } else {
                    self?.state = .failed("daemon exited with status \(finished.terminationStatus)")
                }
            }
        }

        do {
            try process.run()
            self.process = process
            state = .running(pid: process.processIdentifier)
            logger.info("daemon started, pid \(process.processIdentifier)")
        } catch {
            state = .failed(error.localizedDescription)
        }
    }

    func stop() {
        process?.interrupt()  // SIGINT: daemon unregisters targets and exits
    }

    // MARK: - launchd agent (daemon survives app quit, starts at login)

    private let agent = SMAppService.agent(plistName: "com.taunais.iscsikit.daemon.plist")

    var agentStatus: SMAppService.Status { agent.status }

    func installAgent() {
        do {
            try agent.register()
            logger.info("launch agent registered")
        } catch {
            state = .failed("agent install failed: \(error.localizedDescription)")
        }
        objectWillChange.send()
    }

    func removeAgent() {
        do {
            try agent.unregister()
            logger.info("launch agent removed")
        } catch {
            state = .failed("agent removal failed: \(error.localizedDescription)")
        }
        objectWillChange.send()
    }
}
