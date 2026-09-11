#!/usr/bin/env python3
"""Exact check that a userclient-access entitlement authorizes a driver.

usage: check-userclient-access.py (profile|signed|entitlements) PATH DEXT_BUNDLE_ID

  profile       a .provisionprofile; inspects its Entitlements dictionary
  signed        a signed bundle or Mach-O; inspects its signed entitlements
  entitlements  a plain entitlements plist

Exits 0 only when com.apple.developer.driverkit.userclient-access (a string or
an array of strings) contains DEXT_BUNDLE_ID as one literal element. The same
string under another key, a near miss with different punctuation, or a prefix
match does not count; nothing here is a pattern.
"""
import plistlib
import subprocess
import sys

KEY = "com.apple.developer.driverkit.userclient-access"


def load(mode, path):
    if mode == "profile":
        data = subprocess.run(["security", "cms", "-D", "-i", path],
                              check=True, capture_output=True).stdout
        return plistlib.loads(data).get("Entitlements", {})
    if mode == "signed":
        data = subprocess.run(["codesign", "-d", "--entitlements", "-", "--xml", path],
                              check=True, capture_output=True).stdout
        return plistlib.loads(data)
    if mode == "entitlements":
        with open(path, "rb") as f:
            return plistlib.load(f)
    raise SystemExit(__doc__)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    mode, path, want = sys.argv[1:]
    try:
        ents = load(mode, path)
    except Exception as e:  # unreadable file, failed tool, malformed plist
        print(f"error: cannot read {mode} {path}: {e}", file=sys.stderr)
        return 1
    if not isinstance(ents, dict):
        print(f"error: {path}: entitlements are not a dictionary", file=sys.stderr)
        return 1
    value = ents.get(KEY)
    if isinstance(value, str):
        values = [value]
    elif isinstance(value, list):
        values = [v for v in value if isinstance(v, str)]
    else:
        values = []
    if want in values:
        return 0
    print(f"error: {path}: {KEY} = {values!r} does not contain {want!r}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
