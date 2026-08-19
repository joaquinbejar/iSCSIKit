import SwiftUI

@main
struct ISCSIKitApp: App {
    @StateObject private var extensionManager = ExtensionManager()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(extensionManager)
        }
        .windowResizability(.contentSize)
    }
}
