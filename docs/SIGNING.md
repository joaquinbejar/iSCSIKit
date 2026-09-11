# Signing, entitlements and provisioning

Which entitlement goes on which binary, why, and how the kernel decides whether
the daemon may open the driver. Everything here was verified against xnu
(`iokit/Kernel/IOUserServer.cpp`, `IOUserServer::serviceNewUserClient`) and
against the profiles and signatures of a shipped build; it is not inferred from
the portal UI, which is misleading for this capability.

## Who talks to whom

| Binary | Bundle ID | Role | Entitlements |
|---|---|---|---|
| App | `com.taunais.iscsi-initiator` | Installs/activates the dext, launches the daemon | `com.apple.developer.system-extension.install` |
| Daemon | `com.taunais.iscsi-initiator.daemon` (nested `Contents/Library/iSCSIKitDaemon.app`) | Opens the dext's `IOUserClient` with `IOServiceOpen` | `com.apple.developer.driverkit.userclient-access` = `[com.taunais.iscsi-initiator.dext]` |
| Dext | `com.taunais.iscsi-initiator.dext` | Virtual SCSI HBA | `com.apple.developer.driverkit`, `com.apple.developer.driverkit.family.scsicontroller`; development builds add `com.apple.developer.driverkit.allow-any-userclient-access` |

The value of `userclient-access` is the list of **driver** bundle IDs the client
may open. It is never the client's own bundle ID, and the entitlement means
nothing on the dext side (the kernel does not read it there).

## What the kernel actually checks (macOS)

`IOUserServer::serviceNewUserClient` grants the connection when either:

1. the **dext** carries `com.apple.developer.driverkit.allow-any-userclient-access`
   (any value; presence is enough), or
2. the **client process** carries `com.apple.developer.driverkit.userclient-access`
   and one of its elements equals the `CFBundleIdentifier` of the dext's
   IOService (`com.taunais.iscsi-initiator.dext`, visible in `ioreg`).

Anything else returns `kIOReturnNotPermitted` (`0xe00002e2`) from
`IOServiceOpen`. The `communicates-with-drivers` /
`allow-third-party-userclients` pair is compiled only for iOS
(`checkiOS3pEntitlements` is `false` on macOS), so it is not an alternative.

## Development builds

Xcode automatic signing gives the dext a "DriverKit Team Provisioning Profile"
that includes `allow-any-userclient-access`, so the daemon needs no entitlement
at all and can run as a plain executable from `swift build`.

## Developer ID builds

Restricted entitlements are only honored when an embedded provisioning profile
authorizes them; a binary that claims one without a matching profile is killed
by AMFI at launch (exit 137). A bare command-line executable cannot embed a
profile, which is why the daemon ships as a nested app bundle with
`Contents/embedded.provisionprofile` (copied at build time from
`Daemon/signing/DaemonDeveloperID.provisionprofile`, gitignored).

The dext must not ship with `allow-any-userclient-access`: it would let any
local process register targets and feed arbitrary block data to the kernel's
filesystem parsers. Release builds rely on path 2 above.

### The value list is set by Apple

The portal capability "DriverKit UserClient Access" is a plain checkbox. The
array it puts into every profile of the team comes from the entitlement request
Apple approved, and it cannot be edited per App ID. If that list does not
contain the dext's bundle ID, the daemon launches fine but `IOServiceOpen`
fails with `0xe00002e2`. The fix is administrative: ask Apple to add the dext
bundle ID to the team's allowed `userclient-access` values via
<https://developer.apple.com/contact/request/system-extension/>, then
regenerate the daemon profile and rebuild.

### Verifying a build

```sh
# entitlements actually signed into each binary
codesign -d --entitlements - --xml /Applications/iSCSIKit.app | plutil -p -
codesign -d --entitlements - --xml /Applications/iSCSIKit.app/Contents/Library/iSCSIKitDaemon.app | plutil -p -
codesign -d --entitlements - --xml /Applications/iSCSIKit.app/Contents/Library/SystemExtensions/com.taunais.iscsi-initiator.dext.dext | plutil -p -

# what the embedded profiles authorize
security cms -D -i /Applications/iSCSIKit.app/Contents/Library/iSCSIKitDaemon.app/Contents/embedded.provisionprofile | plutil -extract Entitlements xml1 -o - -

# the bundle ID the kernel compares against
ioreg -l -w0 | grep -A3 '"IOUserServerName" = "com.taunais.iscsi-initiator.dext"'
```

The daemon's signed `userclient-access` must list
`com.taunais.iscsi-initiator.dext`, and its embedded profile must contain the
same string; otherwise the build is not functional, regardless of notarization.

## Notarization checklist

- Every Mach-O signed with Developer ID Application, `--options runtime`,
  `--timestamp`; no `get-task-allow` (`CODE_SIGN_INJECT_BASE_ENTITLEMENTS: NO`).
- `libiscsi.11.dylib` is embedded in the daemon bundle and re-signed with the
  same identity (the hardened runtime refuses Homebrew's signature).
- `xcrun notarytool submit --keychain-profile iscsikit-notary --wait`, then
  `xcrun stapler staple`, then `spctl -a -v` on the app.
