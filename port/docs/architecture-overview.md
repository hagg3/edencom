# Architecture Overview (the port layer)

> First written for the web (Emscripten/WASM) build, retired 2026-10-13; rewritten for the native
> targets. The web host (`eden_main.cpp`, the `*_web` shell, `public/*.js`, the `--wrap` levers) is at
> tag `web-port-final` ([web-port-archive](../../docs/web-port-archive.md)).

## Purpose
How the port's own layers sit on top of the engine described in
[`../../docs/architecture-overview.md`](../../docs/architecture-overview.md) — read that first. This
file only covers what the port adds or replaces.

## The engine is (mostly) the real engine
`World`/`Terrain`/`Player`/`Camera`/`Hud`/`Menu`/`FileManager`/`Resources`/`SpecialEffects` and
their ownership graph are compiled from the **same** `Classes/` sources the iOS app was. The small
set replaced instead of compiled (moved to `archive/stock-unbuilt/` in G.1; their seams are in `port/src/seam` and `native/src/seam`) was, with the reason for each: the UIKit shell (`EAGLView.mm`, `EdenViewController.mm`,
`EdenAppDelegate.mm`, `main.m`), `Texture2D.mm`, `FileArchive.mm`, `SimpleAudioEngine.mm`, the stock
world-sharing client (`FileDownload.mm`/`FileUpload.mm`/`SharedList.mm`/`ShareUtil.mm`/`ShareMenu.mm`),
`Alert.mm`, `Appirater.mm`, `md5.c`, `VKeyboard.mm` and `PVRTglesExt.cpp`. Everything else is the
engine, plus the engine files added since the import (`cmake/EngineSources.cmake`).

`Classes/` has been editable since 2026-07-25 under three narrower rules ([`../CLAUDE.md`](../CLAUDE.md));
`git diff pristine-engine -- Classes/` shows exactly what has diverged from the stock 2.1.1 import.

## The lever you reach for

| Lever | What it's for | Example |
|---|---|---|
| **Build flag** | Toggle behaviour already conditional in the code | `EDEN_PLATFORM_IOS`, `EDEN_GL_ES_DIALECT`, `EDEN_USE_SYSTEM_FOUNDATION`, `EDEN_DIAGNOSTICS` |
| **Seam** (`port/src/seam/*_web.*`, `native/src/seam/*_native.*`) | A whole engine file has no portable equivalent (UIKit shell, image decode, audio), or a port-owned C accessor surface | `Texture2D_web.mm`, `Settings_web.mm`, `EdenViewController_native.cpp`, `SimpleAudioEngine_native.mm` |
| **Shim** (`port/src/shim/*`, `native/src/shim/*`) | A whole missing platform layer with no 1:1 engine file | GNU ObjC runtime + Foundation subset (Linux/Windows), GL ES1 → ES3/desktop GL, the GL context/loader |
| **Direct edit** (`Classes/`) | The change is to the *game/engine*, not the platform: perf, correctness, a feature | the mesher and draw path, `.emod` saves, the derived point space, the GL UI kit |

The web build also used link-time `-Wl,--wrap=` on single engine functions; W.1 (2026-10-14) removed
the last of that code. Reach for a direct edit when the same change would be wanted on every target.
"Seam" files keep their `_web` suffix (port/CLAUDE.md); it now means "shared port seam".

## Layers not in the root ownership graph
- `native/src/entry/eden_main_native.cpp` — replaces `main.m`: boot, the interactive SDL loop and every
  `--*` harness mode (docs/execution-flow.md "Native boot and loop").
- `native/src/seam/EdenAppDelegate_native`, `EdenViewController_native`, `main_native`,
  `SDLView_native.mm` — replace the UIKit shell; SDL drives the frame loop, not CADisplayLink.
- `native/src/seam/Input_native.cpp` → `port/src/seam/Input_web.mm` — SDL events become the engine's
  touch slots ([player-input-camera.md](player-input-camera.md)).
- `port/src/shim/objc/` — hand-written ObjC runtime, Linux/Windows only ([objc-runtime.md](objc-runtime.md)).
- `port/src/shim/gl/` + `native/src/shim/gl/` — GL ES 1.1 → ES 3.0/desktop GL ([gl-shim.md](gl-shim.md)).
- `port/src/shim/foundation/` — Foundation subset over stdio for Linux/Windows; on macOS/iOS every
  header defers to Apple's Foundation. `save_backup`, `platform_shims` and `md5_web.mm` are shared by
  all targets.
- `native/src/seam/SimpleAudioEngine_native.mm` + `AppleAudio_native.mm` / `PortableAudio_native.cpp`
  ([resources-and-audio.md](resources-and-audio.md)); `port/src/shim/audio/` holds the failing
  AudioToolbox stubs.
- `native/src/seam/Net_native.cpp` + one backend (`AppleNet`, `NetWinHttp`, `NetCurl`, `NetNone`) —
  the `eden_net_*` seam behind `WorldBrowser` and `WorldShare` (docs/networking.md).
- `port/src/seam/` accessors — settings and keybinds (`Settings_web.mm`), the display profile, menu,
  storage, input, load failure, app lifecycle, dev console: plain C functions the host calls.

## Topics that are unchanged from the root docs
Terrain storage and streaming, worldgen, the `.eden` format, liquids/portals/fireworks and creature AI
are engine code documented in [`../../docs/`](../../docs/README.md), modifications included. Check
`git diff pristine-engine -- Classes/` before relying on a root doc for code you are about to depend on.

## Reading order
1. This file, then [conventions-and-pitfalls.md](conventions-and-pitfalls.md).
2. [`../../native/README.md`](../../native/README.md) — how it's built and run, and the harness modes.
3. [`../../docs/execution-flow.md`](../../docs/execution-flow.md) — boot order and the frame loop.
4. [gl-shim.md](gl-shim.md) if touching rendering; [`../../docs/save-load.md`](../../docs/save-load.md) if
   touching persistence; [ui.md](ui.md) if touching HUD/menu/settings.

## Porting-roadmap note
Root's [`engine-vs-game.md`](../../docs/engine-vs-game.md) sketches an engine/game split as a
*roadmap*. The port realised it pragmatically — seams and shims around the existing engine, not a
rewrite; treat the root file as authoritative for the classification and the lever table above as how
it played out.
