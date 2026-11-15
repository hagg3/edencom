K.3 dependency resolution fixture: `eden_native --content-dump --packs=<this dir>` and
`pack.py compile <this dir> --check LOG` must print the same `## mods` section (CI, linux).
Expected: base, uses, peace, newapi (emod>=2) load; newer (base too old), chain (needs newer), rival (conflicts with uses),
after (needs rival), ghost (not installed), toonew (emod>=99: "needs Emod scripting 99 or newer") are skipped.
