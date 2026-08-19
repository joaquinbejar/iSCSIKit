[![License](https://img.shields.io/badge/license-MIT-blue)](./LICENSE)
[![Platform](https://img.shields.io/badge/platform-macOS%2015%2B%20(Apple%20Silicon)-lightgrey)](https://developer.apple.com/documentation/driverkit)
[![Swift](https://img.shields.io/badge/Swift-6.0%2B-orange)](https://swift.org)
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

The entire userspace stack is implemented and verified against real hardware
(an ASUSTOR NAS with mutual CHAP and header/data digests): discovery, login,
capacity, and raw READ(16) through the same code path `serve` uses. The last
missing link is loading the signed dext so the kernel starts sending CDBs.

- [x] Project skeleton (XcodeGen + SwiftPM)
- [x] Daemon: discovery (SendTargets), login, `READ CAPACITY(16)`, `INQUIRY`
- [x] Dext ↔ daemon transport (IOUserClient: async task notification,
      dequeue/complete with inline payloads)
- [x] Dext: task queue, target registry, data buffer copy in/out
- [x] Daemon: raw CDB passthrough (`iscsikitd serve` pumps kernel CDBs to the
      target over libiscsi)
- [x] CHAP, including mutual (URL carries initiator credentials;
      `LIBISCSI_CHAP_TARGET_USERNAME/PASSWORD` carry the target's), verified
      against real hardware with header/data digests enabled
- [x] Multi-session: `iscsikitd serve` bridges several targets at once
- [x] Reconnection: transparent reconnect + retry on transport errors
- [x] Sleep/wake: IOKit power notifications force a reconnect on wake
- [x] Configuration UI: target list persisted in the app, daemon lifecycle
      (start/stop/log) managed from the Daemon panel
- [x] launchd integration: the app bundles `iscsikitd` and a LaunchAgent
      (`SMAppService`) — "Install Login Agent" keeps sessions alive across
      app quits and reconnects at login, reading the shared target config
- [ ] First end-to-end test with loaded dext (`iscsikitd serve` + Disk Utility)
- [ ] Shared-memory rings (`UserProcessBundledParallelTasks` + mapped command/
      response buffers) instead of copy-per-IO — deliberately deferred until
      the copy path is validated end-to-end

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

### Target URLs

Everything is addressed with libiscsi-native iSCSI URLs:

```
iscsi://[<user>[%<password>]@]<host>[:<port>]/<target-iqn>/<lun>
```

CHAP credentials for the initiator travel inside the URL. For **mutual CHAP**
(the target also authenticates itself to you), export the target's
credentials as environment variables — libiscsi picks them up automatically:

```sh
export LIBISCSI_CHAP_TARGET_USERNAME=mutualuser
export LIBISCSI_CHAP_TARGET_PASSWORD=mutualsecret
```

### Exploring targets (no driver needed)

```sh
# List every target a portal announces (SendTargets discovery)
iscsikitd discover 192.168.1.10
#   iqn.2004-04.com.example:target0
#     portal: 192.168.1.10:3260,1

# Log in and query one LUN: INQUIRY + READ CAPACITY(16)
iscsikitd info 'iscsi://chapuser%secret@192.168.1.10:3260/iqn.2004-04.com.example:target0/0'
#   device: EXAMPLE iSCSI Storage
#   capacity: 8589934592 blocks x 512 B = 4096.0 GiB

# Smoke-test the raw data path: READ(16) of LBA 0 through the exact
# execute() path `serve` uses
iscsikitd verify 'iscsi://chapuser%secret@192.168.1.10:3260/iqn.2004-04.com.example:target0/0'
#   READ(16) OK: 4096 bytes from LBA 0
```

### Bridging LUNs as disks (driver required)

```sh
# 1. Allow development-signed driver extensions (once)
systemextensionsctl developer on

# 2. Launch the app, click "Install Driver", approve it in
#    System Settings › General › Login Items & Extensions

# 3. Bridge one or several LUNs — each appears as a disk in Disk Utility.
#    Multi-target: pass N URLs, they become SCSI targets 0..N-1.
iscsikitd serve \
  'iscsi://chapuser%secret@192.168.1.10/iqn.2004-04.com.example:target0/0' \
  'iscsi://192.168.1.11/iqn.2004-04.com.example:target1/0'
#   target 0: EXAMPLE iSCSI Storage — iqn…target0 lun 0 @ 192.168.1.10:3260, 4096.0 GiB
#   target 1: EXAMPLE iSCSI Storage — iqn…target1 lun 0 @ 192.168.1.11:3260, 512.0 GiB
#   2 target(s) registered — LUNs should appear as disks

# Ctrl-C unregisters the targets and logs out cleanly.
```

While `serve` runs it also handles the ugly parts for you:

- **Transport errors** trigger one transparent reconnect + retry per task
  (`iscsi_force_reconnect_sync`), so a flaky switch doesn't surface as an
  I/O error to the filesystem.
- **Sleep/wake**: the daemon subscribes to IOKit power notifications, never
  vetoes sleep, and force-reconnects every session when the system wakes.

### Managing targets from the app

The app persists a target list and supervises the daemon for you:

1. Add targets in the **Targets** panel (name + iSCSI URL).
2. Point the **Daemon** panel at your `iscsikitd` binary (defaults to
   `/opt/homebrew/bin/iscsikitd`).
3. **Connect All** launches `iscsikitd serve` with every configured target;
   the panel shows the live daemon log. **Disconnect** sends SIGINT, which
   unregisters the targets and logs out.
4. **Install Login Agent** registers the bundled daemon as a launchd agent
   (`SMAppService`): sessions survive quitting the app and reconnect at
   login. The agent runs `iscsikitd serve --config`, reading the same target
   list the app writes to
   `~/Library/Application Support/iSCSIKit/targets.json` (mutual CHAP
   credentials included), and logs to `/tmp/iscsikitd.log`. **Remove Agent**
   unregisters it.

## Project Layout

```
project.yml                 XcodeGen spec (app + dext targets, signing)
App/                        SwiftUI container app (dext activation UI)
Dext/                       DriverKit extension (.iig interfaces + C++)
Daemon/                     SwiftPM package
  Sources/CLibISCSI/          libiscsi system-library shim
  Sources/CISCSIKitShared/    wire protocol shared with the dext
  Sources/ISCSIKitCore/       Swift iSCSI session wrapper
  Sources/iscsikitd/          CLI daemon + IOKit client + session pump
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
