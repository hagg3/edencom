K.3 dependency resolution fixture: `eden_native --content-dump --packs=<this dir>` and
`pack.py compile <this dir> --check LOG` must print the same `## mods` section (CI, linux).
Expected: base, uses, peace load; newer (base too old), chain (needs newer), rival (conflicts with uses),
after (needs rival), ghost (not installed) are skipped.
