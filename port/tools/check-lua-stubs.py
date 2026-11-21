#!/usr/bin/env python3
"""K.8-8: `eden_native --lua-api-dump` must equal the checked-in LuaLS stubs (docs/eden-api.lua).

usage: check-lua-stubs.py <eden_native> <docs/eden-api.lua>
Regenerate with: eden_native --lua-api-dump > docs/eden-api.lua
"""
import subprocess, sys

def main():
    exe, path = sys.argv[1], sys.argv[2]
    r = subprocess.run([exe, "--lua-api-dump"], capture_output=True, text=True)
    if r.returncode != 0:
        print("FAIL: --lua-api-dump exited %d: %s" % (r.returncode, r.stderr.strip()))
        return 1
    with open(path, "r", newline="") as f:
        want = f.read()
    if r.stdout != want:
        print("FAIL: %s is stale; regenerate with: %s --lua-api-dump > %s" % (path, exe, path))
        return 1
    print("OK: %s matches --lua-api-dump" % path)
    return 0

sys.exit(main())
