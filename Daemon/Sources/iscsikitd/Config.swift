import Foundation

/// On-disk daemon configuration, shared with the app: the app's Targets panel
/// writes this file, `iscsikitd serve --config` reads it. Living in
/// Application Support (not the plist) is what lets a static launchd job
/// serve a user-editable target list.
struct DaemonConfig: Codable {
    struct TargetEntry: Codable {
        var name: String?
        var url: String
        /// Mutual CHAP: what the target must prove to us.
        var mutualUsername: String?
        var mutualPassword: String?
    }

    var targets: [TargetEntry]

    /// Lets medium-modifying commands through to the target. Absent or false
    /// means the read-only policy stays on, which is the only safe default
    /// while the write path is being validated: a write that reached the LUN
    /// with a zeroed payload would corrupt it.
    var allowWrites: Bool?

    /// Logs every command with its decoded LBA and the status the target
    /// returned. Noisy, so it is opt-in; indispensable when diagnosing which
    /// command in a sequence actually fails.
    var traceTasks: Bool?

    static var defaultPath: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("iSCSIKit/targets.json")
    }

    static func load(from path: URL = defaultPath) throws -> DaemonConfig {
        let data = try Data(contentsOf: path)
        return try JSONDecoder().decode(DaemonConfig.self, from: data)
    }
}
