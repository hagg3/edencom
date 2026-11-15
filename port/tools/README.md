# port/tools

Gates run in CI; manual tools are run by hand; generators write a checked-in file.
Finished one-off scripts (`bool-modernise`, `sm2-spelling`, `atlas-d2t`, `atlas-h14`,
`eden-format-measure`) live in `archive/tools/` in the private repo.

| Tool | Purpose | Kind |
|---|---|---|
| `gl-call-gate.py` | no engine file calls GL directly | CI |
| `ui-string-gate.py` | UI string rules | CI |
| `objc-surface.py` | fails on a new `.mm`/`.m` (baseline `objc-surface-baseline.txt`) | CI |
| `pod2edm.py --check`, `srclike-pm-ref.py --check` | movement/pod parity checks | CI |
| `pack.py` | content-pack compiler/oracle (`--check`) | CI |
| `mp-replay.py` | multiplayer protocol replay | CI |
| `eden-convert.js`, `eden-convert-test.js`, `convert64-crosscheck.js` | `.eden` converter and cross-check | CI |
| `folders-crash-gate.sh`, `live-cmds-gate.sh`, `hostile-world-gate.sh`, `tutorial-crash-gate.sh` | crash/command/hostile-world/tutorial-install kill-point gates | CI |
| `emod.py` | `.emod` reference reader/writer, `bake-default`, fixtures | CI + generator |
| `dead-exports.py` (+ `dead-exports-allow.txt`) | dead-export check | gate |
| `engine-sources.txt` | the engine source list CMake compiles | data |
| `console-reference.py` | regenerates/`--check`s `docs/console-reference.md` (needs `--console-dump`) | generator, manual |
| `atlas-k13.py`, `ui-icons.py`, `eden-icons.js`, `wonders-pack-art.py`, `example-pack-art.py` | draw atlas tiles, icons, pack art | generator |
| `shot-diff.py`, `power-{leg,report,sampler}`, `folders-{stage,getworlds}-gate.sh`, `mp-live-gate.sh`, `ui-device-shots.sh` | screenshots, power, device and live-server rounds | manual |
| `vendor-lua.sh`, `vendor-zstd.sh` (+ `vendor-lua-emod.patch`) | refresh vendored libraries | manual |
| `ring-walk-gate.sh`, `ring-light-gate.sh` | B.5b-1 ring-streaming walk/light gates (`EDEN_WINDOW_RADIUS`, `CLASSIC=` from the environment) | manual |

## Notes worth knowing before you edit a tool

- **Fixtures:** `fixtures/emod/` is `--emod-selftest`'s cross-target byte gate; `fixtures/classic/` (H.4, 159 columns
  of a 2011 world) is `--classic-selftest`'s 1.x gate; `fixtures/cmds/corpus.txt` is `--console-selftest`'s
  command-block parser corpus (block vs console options); `fixtures/content/stock-dump.txt` is `--content-dump`'s
  pinned text (`.github/scripts/check-content-dump.py` holds its SHA); `fixtures/srclike-pm/vectors.txt` is what
  `--srclike-selftest=move` must match within 1 % (read relative to `native/`).
- **`pack.py` is a mirror:** its JSON reader, validator, mod loader (`compile_mod`, `load_mods`) and dependency
  resolution (`scan_mods`, `resolve_requires`) mirror `Classes/Json.cpp` and `Classes/ContentPack.cpp` line for line,
  messages included. `fixtures/content/packs/` (corrupt packs + `expect.txt`) is read by both `pack.py fixtures` and
  `--pack-selftest`; change both sides together.
- **`emod.py`** needs `zstandard` in a venv. Pack-art scripts need Pillow; run them with `python3 -I`.
- **`tutorial.py`** builds the bundled tutorial world `media/Tutorial.emod` from `tutorial/layout.py` (the stations as
  code; the site survey is in its docstring, `--survey` re-runs it), `tutorial/copy.txt` (every sign and command-block
  line, the file to edit for wording) and `Eden.emod`, through `emod.py`'s writer. The world stores only the columns
  it changes (seed 333333: the rest streams from the bundled map). Output and `tutorial/manifest.txt` (read by
  `--tutorial-selftest`) are a pure function of the inputs: after any edit to layout or copy, run it and commit both.
  `--check` (CI, sanitize job's venv) rebuilds, compares bytes and plants one bad input per refusal (a sign over 95
  characters, a `;` in a `say` line, a script over 511 bytes, a command block off steel, a sign facing a block, a
  floating spawn, a missing copy key). Needs `zstandard` like `emod.py`. Its hint lines (`hint.*` in `copy.txt`) also
  write `Classes/TutorialData.h`, which the hint card reads.
- **`tutorial-crash-gate.sh`** (`<eden_native> <abs scratch dir>`) kills the tutorial install after each staging step
  (`EDEN_TEST_STAGE_KILL_AFTER`), then checks the next launch finishes it exactly once. It drives `--tutorial-selftest
  --tutorial-phase=install|verify`; `--tutorial-mutate=keyfirst` must make it fail.
- **`pod2edm.py`** turns the creature `.pod` art into the shipped `.edm` (format: `docs/entities-and-creatures.md`).
- **Vendored Lua:** `vendor-lua.sh` copies Lua 5.4's core and the sandbox's libraries into `src/shim/vendor/lua/`, then
  applies `vendor-lua-emod.patch` (caps on the C loops a mod script sizes). The vendored `lstrlib.c`/`ltablib.c` are
  **not** upstream's: every hunk says `Emod (QA.3)`. `lua_onefile.cpp` is Emod's, compiling them as one C++ TU.
- **Rehearsing a pack on a real world:** `--live-cmds` on a *copy* of the world under `TESTERS/`, `open <display
  name>` (not the folder id), then `emod.py verify`/`export`.
