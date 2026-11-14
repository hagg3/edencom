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
| `folders-crash-gate.sh`, `live-cmds-gate.sh`, `hostile-world-gate.sh` | crash/command/hostile-world gates | CI |
| `emod.py` | `.emod` reference reader/writer, `bake-default`, fixtures | CI + generator |
| `dead-exports.py` (+ `dead-exports-allow.txt`) | dead-export check | gate |
| `engine-sources.txt` | the engine source list CMake compiles | data |
| `console-reference.py` | regenerates/`--check`s `docs/console-reference.md` (needs `--console-dump`) | generator, manual |
| `atlas-k13.py`, `ui-icons.py`, `eden-icons.js`, `wonders-pack-art.py`, `example-pack-art.py` | draw atlas tiles, icons, pack art | generator |
| `shot-diff.py`, `power-{leg,report,sampler}`, `folders-{stage,getworlds}-gate.sh`, `mp-live-gate.sh`, `ui-device-shots.sh` | screenshots, power, device and live-server rounds | manual |
| `vendor-lua.sh`, `vendor-zstd.sh` (+ `vendor-lua-emod.patch`) | refresh vendored libraries | manual |
