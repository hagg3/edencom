#!/usr/bin/env python3
"""C.10: regenerate the verb and variable tables of docs/console-reference.md from the binary.

usage: console-reference.py <eden_native> [--check] [doc]

Runs `eden_native --console-dump` (Console::referenceMarkdown: the same tables `help` and `cvars` read)
and replaces everything between each `<!-- BEGIN generated:NAME -->` / `<!-- END generated:NAME -->`
pair in the doc. --check changes nothing and exits 1 if the file is out of date. Use a binary built
with -DEDEN_DIAGNOSTICS=ON so the diagnostics verbs are listed. Never edit those regions by hand.
"""
import os
import re
import subprocess
import sys
import tempfile

args = [a for a in sys.argv[1:] if a != "--check"]
check = "--check" in sys.argv[1:]
if not args:
    sys.exit(__doc__)
binary = args[0]
doc = args[1] if len(args) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs", "console-reference.md")

here = os.path.dirname(os.path.abspath(__file__))
scratch = os.path.join(here, "..", "..", "TESTERS")   # working files stay in the repo
os.makedirs(scratch, exist_ok=True)
with tempfile.TemporaryDirectory(dir=scratch) as docs:
    out = subprocess.run([binary, "--console-dump", "--docs=" + docs], capture_output=True, text=True, check=True).stdout
blocks = dict(re.findall(r"<!-- BEGIN generated:(\S+) -->\n(.*?)<!-- END generated:\1 -->\n", out, re.S))
if not blocks:
    sys.exit("console-reference: the binary printed no generated blocks")

text = open(doc, encoding="utf-8").read()
new = text
for name, body in blocks.items():
    pat = re.compile(r"(<!-- BEGIN generated:%s -->\n).*?(<!-- END generated:%s -->)" % (re.escape(name), re.escape(name)), re.S)
    if not pat.search(new):
        sys.exit("console-reference: %s has no generated:%s markers" % (doc, name))
    new = pat.sub(lambda m: m.group(1) + body + m.group(2), new)
if new == text:
    print("console-reference: up to date")
elif check:
    sys.exit("console-reference: %s is out of date; run port/tools/console-reference.py" % doc)
else:
    open(doc, "w", encoding="utf-8").write(new)
    print("console-reference: rewrote", doc)
