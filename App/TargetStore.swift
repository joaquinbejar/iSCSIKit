import Foundation

struct ISCSITarget: Identifiable, Codable, Equatable {
    var id = UUID()
    var name: String
    var url: String  // iscsi://[user[%pass]@]host[:port]/target-iqn/lun
    var mutualUsername: String?
    var mutualPassword: String?
}

/// Persists the configured targets as the daemon's config file
/// (~/Library/Application Support/iSCSIKit/targets.json), so the app's list
/// and `iscsikitd serve --config` — including the launchd agent — always see
/// the same targets.
@MainActor
final class TargetStore: ObservableObject {
    static var configPath: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("iSCSIKit/targets.json")
    }

    @Published var targets: [ISCSITarget] {
        didSet { save() }
    }

    init() {
        // The daemon config is the source of truth; UserDefaults keeps the
        // display names and ids that the JSON schema doesn't carry.
        if let data = UserDefaults.standard.data(forKey: "targets"),
           let decoded = try? JSONDecoder().decode([ISCSITarget].self, from: data) {
            targets = decoded
        } else {
            targets = []
        }
        // No save() here: launching the app must not rewrite a daemon config
        // edited outside the UI. The file is written only on real changes.
    }

    func add(name: String, url: String, mutualUsername: String? = nil,
             mutualPassword: String? = nil) {
        targets.append(ISCSITarget(
            name: name, url: url,
            mutualUsername: mutualUsername?.isEmpty == false ? mutualUsername : nil,
            mutualPassword: mutualPassword?.isEmpty == false ? mutualPassword : nil))
    }

    func remove(_ target: ISCSITarget) {
        targets.removeAll { $0.id == target.id }
    }

    private func save() {
        if let data = try? JSONEncoder().encode(targets) {
            UserDefaults.standard.set(data, forKey: "targets")
        }
        writeDaemonConfig()
    }

    private func writeDaemonConfig() {
        struct Entry: Codable {
            var name: String?
            var url: String
            var mutualUsername: String?
            var mutualPassword: String?
        }

        let path = Self.configPath
        let entries = targets.map {
            Entry(name: $0.name, url: $0.url,
                  mutualUsername: $0.mutualUsername, mutualPassword: $0.mutualPassword)
        }
        do {
            try FileManager.default.createDirectory(
                at: path.deletingLastPathComponent(), withIntermediateDirectories: true)
            // Preserve keys the UI does not manage (allowWrites, traceTasks…).
            var root: [String: Any] = [:]
            if let existing = try? Data(contentsOf: path),
               let object = try? JSONSerialization.jsonObject(with: existing) as? [String: Any] {
                root = object
            }
            let encoded = try JSONEncoder().encode(entries)
            root["targets"] = try JSONSerialization.jsonObject(with: encoded)
            let data = try JSONSerialization.data(withJSONObject: root,
                                                  options: [.prettyPrinted, .sortedKeys])
            try data.write(to: path, options: .atomic)
        } catch {
            NSLog("iSCSIKit: failed to write daemon config: \(error)")
        }
    }
}
