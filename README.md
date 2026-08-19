[![License](https://img.shields.io/badge/license-MIT-blue)](./LICENSE)
[![Platform](https://img.shields.io/badge/platform-macOS%2015%2B%20(Apple%20Silicon)-lightgrey)](https://developer.apple.com/documentation/driverkit)
[![Swift](https://img.shields.io/badge/Swift-5.9%2B-orange)](https://swift.org)
[![Stars](https://img.shields.io/github/stars/joaquinbejar/iSCSIKit.svg)](https://github.com/joaquinbejar/iSCSIKit/stargazers)
[![Issues](https://img.shields.io/github/issues/joaquinbejar/iSCSIKit.svg)](https://github.com/joaquinbejar/iSCSIKit/issues)
[![PRs](https://img.shields.io/github/issues-pr/joaquinbejar/iSCSIKit.svg)](https://github.com/joaquinbejar/iSCSIKit/pulls)

# iSCSIKit

Modern iSCSI initiator for Apple Silicon Macs — DriverKit-based, no kexts.

## Why

macOS has never shipped a native iSCSI initiator, and every third-party option
(ATTO Xtend SAN, globalSAN, the old kext-based `iscsi-osx/iSCSIInitiator`) is
dead or incompatible with Apple Silicon. iSCSIKit fills that gap using Apple's
sanctioned userspace driver stack: a
[SCSIControllerDriverKit](https://developer.apple.com/documentation/scsicontrollerdriverkit)
extension presents remote LUNs as regular disks, while a userspace daemon
speaks the iSCSI protocol (RFC 7143) over TCP.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│ macOS storage stack (Disk Utility, APFS, /dev/diskN)    │
└──────────────────────────▲──────────────────────────────┘
                           │ SCSI CDBs
┌──────────────────────────┴──────────────────────────────┐
│ iSCSIKitDext — virtual HBA (SCSIControllerDriverKit)    │
│ App/Contents/Library/SystemExtensions                   │
└──────────────────────────▲──────────────────────────────┘
                           │ IOUserClient (async notify + dequeue/complete)
┌──────────────────────────┴──────────────────────────────┐
│ iscsikitd — userspace daemon, iSCSI over TCP (libiscsi) │
└──────────────────────────▲──────────────────────────────┘
                           │ TCP 3260
                    ┌──────┴──────┐
                    │ iSCSI target│  (NAS, SAN, tgt, LIO…)
                    └─────────────┘
```

The kernel builds every CDB; iSCSIKit only transports them. The dext queues
`SCSIUserParallelTask`s and notifies the daemon through an async IOUserClient
callback; the daemon dequeues each task (CDB + write payload inline), executes
it against the target with libiscsi's raw passthrough, and completes it back
(status + sense + read payload inline).

- **App/** — SwiftUI container app. Installs/activates the dext via
  `SystemExtensions.framework`.
- **Dext/** — `IOUserSCSIParallelInterfaceController` subclass plus the
  `IOUserClient` the daemon connects to.
- **Daemon/** — SwiftPM package. `iscsikitd` (CLI + pump) and `ISCSIKitCore`
  (Swift wrapper over [libiscsi](https://github.com/sahlberg/libiscsi):
  discovery, login, CHAP, raw CDB execution). Fully testable without any
  entitlement.

## Status

Functional skeleton, end-to-end data path implemented but not yet tested with
a loaded dext (needs the DriverKit entitlements + a signed build).

- [x] Project skeleton (XcodeGen + SwiftPM)
- [x] Daemon: discovery (SendTargets), login, `READ CAPACITY(16)`, `INQUIRY`
- [x] Dext ↔ daemon transport (IOUserClient: async task notification,
      dequeue/complete with inline payloads)
- [x] Dext: task queue, target registry, data buffer copy in/out
- [x] Daemon: raw CDB passthrough (`iscsikitd serve` pumps kernel CDBs to the
      target over libiscsi)
- [ ] First end-to-end test with loaded dext (`iscsikitd serve` + Disk Utility)
- [ ] CHAP in `serve` (core supports it; CLI flag missing)
- [ ] Reconnection, multi-session, sleep/wake handling
- [ ] Shared-memory rings (`UserProcessBundledParallelTasks`) instead of
      copy-per-IO
- [ ] Configuration UI (targets live only in the CLI for now)

> **Warning**: pre-alpha storage software. Do not point it at data you care
> about, and never connect a second initiator to a LUN that is already
> mounted elsewhere — block devices without a cluster filesystem corrupt.

## Requirements

- Apple Silicon Mac, macOS 15+
- Xcode 26+, [XcodeGen](https://github.com/yonaskolb/XcodeGen), libiscsi
  (`brew install xcodegen libiscsi`)
- An Apple Developer Program membership with the DriverKit development
  capabilities enabled on the dext's App ID (self-service under
  *Identifiers › Additional Capabilities*):
  `com.apple.developer.driverkit`,
  `com.apple.developer.driverkit.family.scsicontroller`,
  `com.apple.developer.driverkit.allow-any-userclient-access`

## Build

```sh
# Daemon (works today, no entitlements needed)
cd Daemon
swift build && swift test
.build/debug/iscsikitd discover 192.168.1.10

# App + dext (signed; set DEVELOPMENT_TEAM in project.yml to your team)
xcodegen generate
xcodebuild -project iSCSIKit.xcodeproj -scheme iSCSIKit build
```

## Usage

```sh
# 1. Allow development-signed driver extensions (once)
systemextensionsctl developer on

# 2. Launch the app, click "Install Driver", approve it in
#    System Settings › General › Login Items & Extensions

# 3. Explore the target
iscsikitd discover 192.168.1.10
iscsikitd info 192.168.1.10:3260 iqn.2004-04.com.example:target0 0

# 4. Bridge a LUN — it appears as a disk in Disk Utility
iscsikitd serve 192.168.1.10:3260 iqn.2004-04.com.example:target0 0
```

## Project Layout

```
project.yml                 XcodeGen spec (app + dext targets, signing)
App/                        SwiftUI container app (dext activation UI)
Dext/                       DriverKit extension (.iig interfaces + C++)
Daemon/                     SwiftPM package
  Sources/CLibISCSI/          libiscsi system-library shim
  Sources/CISCSIKitShared/    wire protocol shared with the dext
  Sources/ISCSIKitCore/       Swift iSCSI session wrapper
  Sources/iscsikitd/          CLI daemon + IOKit client + task pump
  Tests/                      unit tests
```

## Contribution and Contact

We welcome contributions to this project! If you would like to contribute,
please follow these steps:

1. Fork the repository.
2. Create a new branch for your feature or bug fix.
3. Make your changes and ensure that the project still builds and all tests pass.
4. Commit your changes and push your branch to your forked repository.
5. Submit a pull request to the main repository.

If you have any questions, issues, or would like to provide feedback, please
feel free to contact the project maintainer:

### **Contact Information**
- **Author**: Joaquín Béjar García
- **Email**: jb@taunais.com
- **Telegram**: [@joaquin_bejar](https://t.me/joaquin_bejar)
- **Repository**: <https://github.com/joaquinbejar/iSCSIKit>

We appreciate your interest and look forward to your contributions!

**License**: MIT
