# The port layer — reference documentation

Topic reference for `port/src/` (the shared seam and shim code every native target compiles) and the
pieces of the engine it reaches into. Each file reads as a **delta** on the engine docs in
[`../../docs/`](../../docs/README.md): it covers what the port adds or replaces and defers to the root
doc for everything else.

These pages were first written for the web (Emscripten/WASM) build, as `web/docs/`. **The web build
was retired on 2026-10-13**; its web-only parts were cut (W.5 and the 2026-10-14 docs pass) and each
page's one-line header says where they are — tag `web-port-final`, public snapshot hagg3/edence;
[`../../docs/web-port-archive.md`](../../docs/web-port-archive.md) lists what was dropped and which
native check replaced each web test.

| Location | What it is | Read it for |
|---|---|---|
| `port/docs/` (here) | Durable reference for the port layer | "how does X work" |
| [`../../README.md`](../../README.md) | Building, and running the tests (`eden_native --help` lists every test mode) | "how do I run X" |
| [`../../docs/`](../../docs/README.md) | Engine documentation | engine behaviour, the formats, worldgen, boot and loop |

## Index
- [architecture-overview.md](architecture-overview.md) — the port's layers over the engine and the
  build-flag/seam/shim/edit lever table. **Start here.**
- [conventions-and-pitfalls.md](conventions-and-pitfalls.md) — the port's own hard-won rules.
- [objc-runtime.md](history/objc-runtime.md) — the hand-written GNU gnustep-1.9 runtime (Linux, Windows; the
  `-dealloc` rule).
- [gl-shim.md](history/gl-shim.md) — *history (deleted in G.5b-7)*: the GL ES 1.1 → ES 3.0 / desktop GL translator;
  its texture-upload, streaming and context notes still describe code that moved to `port/src/render/gl/`.
- [renderer.md](renderer.md) — the engine-owned renderer (`Classes/Renderer.h`, G.5): backends, the
  shared GL state cache and lazy-apply bridge, the five programs, and the instruments (`--scene-shot`,
  `--state-trace`, `--render-selftest`, `shot-diff.py`).
- [design-system.md](design-system.md) — the visual language the GL widget kit (`Classes/GLWidgets`)
  transcribes: tokens, the three mechanics, type, icons.
- [player-input-camera.md](player-input-camera.md) — SDL input → the shared translator
  (`src/seam/Input.cpp`), native keyboard/touch/gamepad notes, crouch, the eyedropper.
- [resources-and-audio.md](resources-and-audio.md) — the native audio backend (SDL mixer, Apple and
  portable decoders, five channels), `Texture2D.cpp` (PNG decode, the recolour pipeline), asset
  packaging.
- [ui.md](ui.md) — the settings and keybind tables (`Settings.cpp`), the ignored retina swap, the
  derived point space and its invariants.
