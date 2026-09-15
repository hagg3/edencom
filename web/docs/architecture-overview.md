# Architecture Overview (Web Port)

## Purpose
How the WASM port's own layers sit on top of the engine described in
[`../../docs/architecture-overview.md`](../../docs/architecture-overview.md) — read that
first, as **legacy iOS documentation**: accurate on engine structure, written for the
original Xcode target, which is no longer built. This file only covers what the web port
adds or replaces.

## The engine is (mostly) unmodified
`World`/`Terrain`/`Player`/`Camera`/`Hud`/`Menu`/`FileManager`/`Resources`/
`SpecialEffects` and their ownership graph are compiled from the **same** `Classes/`
sources as the iOS build (see `CMakeLists.txt`'s `EDEN_SEAM_EXCLUDE` list for the small
set that are replaced instead of compiled — currently: `EAGLView.mm`,
`EdenViewController.mm`, `EdenAppDelegate.mm`, `main.m`, `Texture2D.mm`,
`FileArchive.mm`, `SimpleAudioEngine.mm`, the world-sharing client
(`FileDownload.mm`/`FileUpload.mm`/`SharedList.mm`/`ShareUtil.mm`/`ShareMenu.mm`/
`Alert.mm`), `Appirater.mm`, `md5.c`). Everything else — including
`FileManager.mm`/`FileManagerHelper.mm`, the whole voxel/terrain/creature/liquid/
lighting stack, and `SettingsMenu.mm`'s data model — is the real engine code.

Through pass 36 that was guaranteed by an absolute rule (`../Classes/` and the root `.mm`
files are never edited; every fix a build flag, a `src/seam/` replacement, a `-Wl,--wrap=`
on a cross-TU symbol, or a `src/shim/` implementation). **That rule was retired on
2026-07-25** — see [`../CLAUDE.md`](../CLAUDE.md) for the three narrower rules that
replaced it. Engine code is now editable for *game and engine* work; the levers below are
still how *platform* differences are handled, and they remain the default reach. The
guarantee is now empirical rather than structural: `git diff pristine-engine -- Classes/`
shows exactly what has diverged from the stock 2.1.1 import.

## The lever you reach for

| Lever | What it's for | Example |
|---|---|---|
| **Build flag** | Toggle behaviour already conditional in the engine | `TARGET_OS_IPHONE`/`BUILD_OGLES` (inherited, not a porting choice) |
| **Seam** (`src/seam/*_web.*`) | Whole file has no web equivalent (UIKit shell, image decode) or the port intentionally wants a from-scratch replacement | `EAGLView_web.mm`, `Texture2D_web.mm`, `Settings_web.mm` |
| **Wrap** (`-Wl,--wrap=`) | One function inside an otherwise-kept engine file needs different behaviour, without forking the whole file | `Player::setSpeed`/`preupdate` (`Movement_web.mm`), `gluPerspective` (FOV setting), `CPVRTglesExt::LoadExtensions` (`pvrt_matrix_palette.cpp`) |
| **Shim** (`src/shim/*`) | A whole missing platform layer (no 1:1 engine file to replace) | ObjC runtime, GL ES1→WebGL2, Foundation subset, Web Audio |
| **Direct edit** (`Classes/`, since 2026-07-25) | The change is to the *game/engine*, not to the platform: perf, correctness, a feature | Mesher/draw-path optimisation, save atomicity in `FileManager.mm`, unpinning the display constants, deleting the `SettingsMenu::load` stomp |

The first four are still the right answer for anything that exists *because the target is a
browser*. Reach for a direct edit when the same change would be wanted on any target — and
note that several existing wraps (`SettingsMenu::update`/`render`, `gluPerspective`) are now
candidates for retirement in favour of the edits they were standing in for.

`CMakeLists.txt`'s `EDEN_SEAM_EXCLUDE` block is the source of truth for which lever was
used where, with a comment at each entry explaining why. See
[build-and-toolchain.md](build-and-toolchain.md) for the decision test for seam vs. wrap.

## New layers not in the root ownership graph
- `src/entry/eden_main.cpp` — replaces `main.m`; no `UIApplicationMain`.
- `src/seam/EdenAppDelegate_web.{h,cpp}` + `EdenViewController_web.{h,cpp}` +
  `EAGLView_web.mm` — replace the UIKit shell. There is no CADisplayLink; Emscripten
  drives the frame loop instead (see [execution-flow.md](execution-flow.md)).
- `src/shim/objc/` — hand-written ObjC runtime (Emscripten ships none); see
  [objc-runtime.md](objc-runtime.md).
- `src/shim/gl/gl_es1_shim.{h,cpp}` — GL ES 1.1 → WebGL2; see [gl-shim.md](gl-shim.md).
- `src/shim/foundation/` — Foundation subset (NSObject/NSString/NSData/NSFileHandle/
  NSFileManager/NSBundle/NSUserDefaults/…) over stdio + IDBFS; see
  [save-load.md](save-load.md) and [third-party.md](third-party.md).
- `src/shim/audio/`, `src/seam/SimpleAudioEngine_web.mm` — see
  [resources-and-audio.md](resources-and-audio.md).
- `public/*.js` — the DOM host: canvas + input listeners (`eden-st.html`), settings
  panel (`eden-settings.js`), pause menu (`eden-pausemenu.js`), IDBFS mount
  (`eden-storage.js`). See [ui.md](ui.md).

## Topics that are unchanged from the root docs
World representation/terrain, worldgen, the `.eden` file format, liquids/portals/
fireworks, and creature AI/save logic are (as of the rule's retirement) still unmodified
engine code — see the per-topic web docs for the (usually short) delta, if any, and the
root docs for everything else. Because engine edits are now permitted, this list is a
statement about *today*, not a guarantee: check `git diff pristine-engine -- Classes/`
before relying on a root doc describing code you're about to depend on.

## Reading order
1. This file, then [conventions-and-pitfalls.md](conventions-and-pitfalls.md).
2. [build-and-toolchain.md](build-and-toolchain.md) — how it's built and run.
3. [execution-flow.md](execution-flow.md) — boot order and the frame loop.
4. [gl-shim.md](gl-shim.md) if touching rendering; [save-load.md](save-load.md) if
   touching persistence; [ui.md](ui.md) if touching HUD/menu/settings.
5. Whatever subsystem you're touching — check its web doc for a delta before assuming
   the root doc is the whole story.

## Porting-roadmap note
Root's [`engine-vs-game.md`](../../docs/engine-vs-game.md) sketches a hypothetical
engine/game split as a *roadmap*, not something done. This port is that roadmap in
motion, but pragmatically (seam/wrap/shim around the existing engine, not a rewrite) —
there is no separate web/docs equivalent; treat the root file as still authoritative
for the abstract classification, and this file's "lever" table as how it played out
in practice.
