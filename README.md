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

Verified end to end on Apple Silicon macOS 26 with a notarized build against
an ASUSTOR NAS (mutual CHAP): the driver loads, the daemon opens it, and the
LUN appears as a disk. Reads work through the whole stack. Formatting with
`diskutil eraseDisk` still receives zero-filled write payloads in the dext,
although a raw-device write delivered nonzero data. The daemon therefore
keeps read-only access enabled by default.

- [x] Discovery, login, CHAP and mutual CHAP, multi-session, reconnection,
      sleep/wake, configuration UI, launchd login agent
- [x] Dext ↔ daemon transport (async task notification, dequeue/complete)
      with an asynchronous pump: dozens of commands in flight per session
- [x] Developer ID signing, notarization, DMG; entitlement model documented
      in [docs/SIGNING.md](docs/SIGNING.md)
- [x] Read-only enforcement: default-deny CDB allowlist with service-action
      checks, DATA PROTECT sense for anything else
- [ ] **Writes and formatting**: on Apple Silicon macOS 26, the measured
      `diskutil eraseDisk` writes arrived zero-filled, while a raw
      `dd` to `/dev/rdiskN` delivered nonzero payload
      ([docs/WRITE-PATH-INVESTIGATION.md](docs/WRITE-PATH-INVESTIGATION.md)),
      reported to Apple as **FB24799838**, DTS **22255765**. Validation on
      macOS 27 remains pending.
- [ ] Larger tasks are not possible: `SCSIUserParallelTask` carries a single
      buffer address and a virtual controller has no DART, so a task is one
      16 KiB page and throughput comes from queue depth
      ([docs/BENCHMARKS.md](docs/BENCHMARKS.md))

> **Warning**: pre-alpha storage software. Do not point it at data you care
> about, and never connect a second initiator to a LUN that is already
> mounted elsewhere — block devices without a cluster filesystem corrupt.

## Install a prebuilt app

Download the notarized DMG from the
[latest release](https://github.com/joaquinbejar/iSCSIKit/releases), drag
iSCSIKit to Applications, launch it, click "Install Driver" and approve it in
System Settings, then add your target and Connect All. The preview is
read-only by design (see Status). `docs/SIGNING.md` explains how to verify
the signatures and entitlements of any build.

## Performance

Sequential throughput over gigabit: the transport reaches ~83 MiB/s read /
~50 MiB/s write. Through the driver, each task is one 16 KiB page (a
SCSIControllerDriverKit limit for virtual controllers), so throughput scales
with tasks in flight: a single raw `dd` stream gets ~9.7 MB/s, eight
concurrent readers ~47 MB/s. Full tables and analysis in
[docs/BENCHMARKS.md](docs/BENCHMARKS.md).

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
