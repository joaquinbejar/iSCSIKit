// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "iSCSIKitDaemon",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "iscsikitd", targets: ["iscsikitd"]),
        .library(name: "ISCSIKitCore", targets: ["ISCSIKitCore"]),
    ],
    targets: [
        .systemLibrary(name: "CLibISCSI", pkgConfig: "libiscsi"),
        // Wire protocol shared with the dext (Dext target includes the same
        // header via HEADER_SEARCH_PATHS in project.yml).
        .target(name: "CISCSIKitShared"),
        .target(
            name: "ISCSIKitCore",
            dependencies: ["CLibISCSI"],
            linkerSettings: [.unsafeFlags(["-L/opt/homebrew/lib"])]
        ),
        .executableTarget(
            name: "iscsikitd",
            dependencies: ["ISCSIKitCore", "CISCSIKitShared"],
            linkerSettings: [.linkedFramework("IOKit")]
        ),
        .testTarget(
            name: "ISCSIKitCoreTests",
            dependencies: ["ISCSIKitCore"]
        ),
    ]
)
