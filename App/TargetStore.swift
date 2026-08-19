import Foundation

struct ISCSITarget: Identifiable, Codable, Equatable {
    var id = UUID()
    var name: String
    var url: String  // iscsi://[user[%pass]@]host[:port]/target-iqn/lun
}

/// Persists the configured targets in UserDefaults.
@MainActor
final class TargetStore: ObservableObject {
    private static let key = "targets"

    @Published var targets: [ISCSITarget] {
        didSet { save() }
    }

    init() {
        if let data = UserDefaults.standard.data(forKey: Self.key),
           let decoded = try? JSONDecoder().decode([ISCSITarget].self, from: data) {
            targets = decoded
        } else {
            targets = []
        }
    }

    func add(name: String, url: String) {
        targets.append(ISCSITarget(name: name, url: url))
    }

    func remove(_ target: ISCSITarget) {
        targets.removeAll { $0.id == target.id }
    }

    private func save() {
        if let data = try? JSONEncoder().encode(targets) {
            UserDefaults.standard.set(data, forKey: Self.key)
        }
    }
}
