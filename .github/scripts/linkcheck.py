#!/usr/bin/env python3
"""QA.26: link check over the PUBLIC reading set (what publish-to-edencom.sh copies).

Every relative markdown link in a public .md file must resolve to a file or folder that is itself
public: a link into WORKING/, TESTERS/, archive/, a CLAUDE.md, native/README.md or
port/docs/README.md is a dead link on edencom. A `/Users/` path anywhere in public text or source is
a hard fail (leaks a local username). Run from anywhere: `linkcheck.py [repo-root]`.
"""
import os, re, sys

root = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", ".."))

PRIVATE_DIRS = ("WORKING", "TESTERS", "archive", ".claude")
PRIVATE_FILES = {"CLAUDE.md", "native/README.md"}   # CLAUDE.md at any depth is private


def rel(p):
    return os.path.relpath(p, root).replace(os.sep, "/")


def is_private(r):
    parts = r.split("/")
    if parts[0] in PRIVATE_DIRS:
        return True
    if parts[-1] == "CLAUDE.md" or r in PRIVATE_FILES:
        return True
    return False


def public_md():
    out = ["README.md"]
    for top in ("docs", "port/docs"):
        for d, _, fs in os.walk(os.path.join(root, top)):
            for f in fs:
                if f.endswith(".md"):
                    out.append(rel(os.path.join(d, f)))
    return sorted(set(out))


LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
bad = []
for f in public_md():
    text = open(os.path.join(root, f), encoding="utf-8").read()
    in_fence = False
    for n, line in enumerate(text.split("\n"), 1):
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
        if in_fence:
            continue
        for m in LINK.finditer(line):
            t = m.group(1).split("#")[0]
            if not t or re.match(r"^[a-z]+:", t):
                continue
            target = rel(os.path.normpath(os.path.join(root, os.path.dirname(f), t)))
            if is_private(target):
                bad.append(f"{f}:{n}: link to private path {target}")
            elif not os.path.exists(os.path.join(root, target)):
                bad.append(f"{f}:{n}: broken link {target}")

# /Users/ in public text and source: tracked files only (build trees and logs are not published)
import subprocess
try:
    tracked = subprocess.run(["git", "-C", root, "ls-files", "-z"], capture_output=True, check=True
                             ).stdout.decode("utf-8", "replace").split("\0")
except Exception:
    tracked = []
skip_ext = {".png", ".bin", ".gz", ".emod", ".eden", ".zip", ".ogg", ".wav", ".edm", ".a", ".o", ".zst",
            ".ipa", ".mp3", ".caf", ".jpg", ".icns", ".ico"}
for r in tracked:
    if not r or not r.split("/")[0] in ("README.md", "docs", "port", "native", "Classes"):
        continue
    if is_private(r) or os.path.splitext(r)[1] in skip_ext or r.startswith("port/tools/fixtures/"):
        continue
    if r == ".github/scripts/linkcheck.py":
        continue
    fp = os.path.join(root, r)
    if not os.path.isfile(fp):
        continue
    try:
        for n, line in enumerate(open(fp, encoding="utf-8"), 1):
            if "/Users/" in line:
                bad.append(f"{r}:{n}: absolute /Users/ path")
    except (UnicodeDecodeError, OSError):
        pass

for b in bad:
    print("linkcheck:", b)
print(f"linkcheck: {len(bad)} problem(s) in {len(public_md())} public markdown files")
sys.exit(1 if bad else 0)
