#!/usr/bin/env python3
"""pick-simulator.py — print the UDID of an iOS simulator to run the gates on.

Phase N Stage 4.1. `xcrun simctl boot "iPhone 15"` is the obvious line and it is wrong on a
runner: the image's device names and installed runtimes change between macos-14, macos-15 and
whatever `macos-latest` points at next month, and a name that has drifted fails as
"Invalid device: iPhone 15" three steps before anything about this port is exercised.

So: ask for what is actually there, and take the newest iOS runtime's first available iPhone.
Newest rather than oldest deliberately — this job's claim is "the build runs on iOS", and the
deployment-target claim (12.0) is asserted out of the DEVICE binary's load command in the same
job, where it is a property of the artefact rather than of whichever runtime Apple ships today.
No simulator below iOS 15 exists in current Xcode at all, which is precisely why criterion 1 is
owed to a human with an old device.

    usage: pick-simulator.py            # reads `simctl list devices available -j` itself
           simctl list ... | pick-simulator.py -
"""

import json
import subprocess
import sys


def runtime_key(runtime):
    """Sort key for `com.apple.CoreSimulator.SimRuntime.iOS-17-5` -> (17, 5)."""
    tail = runtime.rsplit(".", 1)[-1]
    parts = [p for p in tail.split("-") if p.isdigit()]
    return tuple(int(p) for p in parts) or (0,)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "-":
        raw = sys.stdin.read()
    else:
        raw = subprocess.run(["xcrun", "simctl", "list", "devices", "available", "-j"],
                             check=True, capture_output=True, text=True).stdout

    devices = json.loads(raw)["devices"]
    candidates = []
    for runtime, devs in devices.items():
        if "iOS" not in runtime:
            continue
        for d in devs:
            if d.get("isAvailable") and "iPhone" in d.get("name", ""):
                candidates.append((runtime_key(runtime), runtime, d["name"], d["udid"]))

    if not candidates:
        sys.exit("pick-simulator: no available iOS iPhone simulator on this machine")

    candidates.sort()
    key, runtime, name, udid = candidates[-1]
    print(f"pick-simulator: {name} on {runtime}", file=sys.stderr)
    print(udid)


if __name__ == "__main__":
    main()
