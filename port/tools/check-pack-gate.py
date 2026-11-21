#!/usr/bin/env python3
"""K.8-8: `eden_native --check-pack=DIR` exits 0 on the example pack and 1, naming file:line, on a pack whose script has a syntax error.

usage: check-pack-gate.py <eden_native> <fixtures dir (port/tools/fixtures)>
"""
import subprocess, sys

def run(exe, d):
    return subprocess.run([exe, "--check-pack=" + d], capture_output=True, text=True)

def main():
    exe, fx = sys.argv[1], sys.argv[2]
    good = run(exe, fx + "/packs/emod_example")
    bad = run(exe, fx + "/content/mod-badlua")
    ok = True
    if good.returncode != 0:
        print("FAIL: the example pack exits %d: %s" % (good.returncode, good.stdout.strip())); ok = False
    if bad.returncode != 1 or "scripts/main.lua:2: syntax error" not in bad.stdout:
        print("FAIL: the syntax-error pack exits %d, output: %s" % (bad.returncode, bad.stdout.strip())); ok = False
    if ok:
        print("OK: --check-pack passes the example pack and refuses the syntax-error pack with its line")
    return 0 if ok else 1

sys.exit(main())
