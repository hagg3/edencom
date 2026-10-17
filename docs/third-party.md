# Third-Party & Vendored Code

Roughly half the files in `Classes/` are vendored libraries. Treat them as read-only
dependencies; none have been meaningfully forked except where noted. **Not everything here is
compiled by the port**: what builds is `port/tools/engine-sources.txt` minus `native/CMakeLists.txt`'s
`EDEN_NATIVE_SEAM_EXCLUDE` (which drops `SimpleAudioEngine.mm`, `Texture2D.mm`, `md5.c`, `Appirater.mm`
and `PVRTglesExt.cpp`, among others) plus `cmake/EngineSources.cmake`.

| Component | Files | Role | Notes |
|---|---|---|---|
| **PowerVR SDK (PVRT tools)** | `PVRT*.cpp/.h`, `PVRShell*` | POD model loading/animation (`CPVRTModelPOD` — creatures), math types (`PVRTVec/Mat/Quaternion`), PVR texture support, tri-stripping, bone batching | Only the model/math/texture parts are actually used; PVRShell/Print3D are along for the ride (`PVRShell.cpp`/`PVRShellOS.cpp` are not compiled). *Port:* `PVRTglesExt.cpp` is replaced by `native/src/seam/pvrt_matrix_palette_native.cpp`, which supplies the matrix-palette entry points the shim implements. Imagination Technologies license headers apply. |
| **CocosDenshion** | `CocosDenshion.*`, `CDAudioManager.*`, `CDOpenALSupport.*`, `CDConfig.h`, `SimpleAudioEngine*` | OpenAL audio engine (effects) + AVAudioPlayer (music) | Stock links it as the prebuilt `libCocosDenshion.a`; the C++ wrapper (`SimpleAudioEngine.mm`, namespace `CocosDenshion::`) sits over it. *Port:* none of it is compiled; `Classes/SimpleAudioEngine.h` stays the contract and `native/src/seam/SimpleAudioEngine_native.mm` implements it ([resources-and-audio.md](resources-and-audio.md)). |
| **cocos2d fragments** | `CCDirector/CCCamera/CCScheduler/CCConfiguration.cpp`, `ccFPSImages.c` | Support code the C++ SimpleAudioEngine port dragged in | Not a real cocos2d integration — the game has its own loop. Not compiled by the port. |
| **Texture2D** | `Texture2D.mm/.h`, `OpenGL_Internal.h` | Apple's classic texture + string-texture class | Extended with `drawSky` etc. — this one *is* locally modified. *Port:* seam-excluded; `port/src/seam/Texture2D_web.mm` (stb_image decode) and `native/src/seam/TextRaster_native.cpp` (string raster) replace it. |
| **MESA GLU port** | `glu.h`, `glue.c`, `project.c`, `registry.c`, `error.c`, `gluos.h`, `gluint.h`, `docs/reference/la-map.c` | `gluPerspective`, `gluUnProject` (block picking), `gluLookAt` | SGI Free Software License B headers. |
| **zlib helper** | `zpipe.c/.h` | Deflate/inflate streams | Only user was the disabled `FileArchive` + `gzipInflate`. |
| **zstd 1.5.7** | `zstd/zstd.c`, `zstd.h`, `zstd_errors.h`, `LICENSE` | Per-column codec for `.emod` (`EdenWorldStore`) | **Added after the import.** Single-file amalgamation, BSD-3-Clause (take BSD, not GPLv2; the notice in `LICENSE` must ship in binary distributions). ONE pinned copy on every target — byte-identical `.emod` output depends on it. Built without the dictionary builder, threading, legacy decoders and asm; CMake (`cmake/EngineSources.cmake`) also compiles out the strategies above level 3's (dfast), so **the engine encodes only at the low levels** (level 19 is `emod.py`'s job). Regenerate with `port/tools/vendor-zstd.sh`. Native include path puts `Classes/zstd/` first so Homebrew's header is never picked up. |
| **Lucide icons** | `media/ui/ui_icons.png`, `Classes/UIIcons.h`, `media/ui/LUCIDE-LICENSE.txt` | GL UI icon atlas (12 glyphs), rasterised offline by `port/tools/ui-icons.py` from the paths in `port/tools/eden-icons.js` | ISC; the notice ships beside the atlas. Generated: edit the tool's `ICONS` list, never the PNG/header. |
| **md5** | `md5.c/.h` | Preview-screenshot hash | Public-domain RSA-derivative implementation. *Port:* `port/src/shim/foundation/md5_web.mm` replaces it. |
| **hashmap** | `hashmap.mm/.h` | Int-keyed open-addressing hashmap | Generic; used for save-file directories. Small enough to be considered first-party by now. |
| **Appirater** | `Appirater.mm/.h` | "Rate this app" prompt | Called from the app delegate. Not compiled by the port. |
| **Flurry** | `[Flurry startSession:...]` in the delegate | Analytics | The binary lib (`libFlurry_6.0.0.a`) the Xcode project names is not in this repo, so the stock target cannot link as-is. Session key is hard-coded in `EdenAppDelegate.mm` (seam-excluded on the port). |
| **TestFlight** | `Eden/libTestFlight.a` | Beta crash reporting | All call sites commented out; the .a remains linked in the project. |

## Non-compiled material in the repo
- `docs/reference/MROB.txt`, `docs/reference/Eden_file_format.txt` — file-format documentation (see
  [eden-file-format.md](eden-file-format.md)).
- `docs/reference/EdenDesignDoc.pdf`, `docs/reference/sound_design_draft.pdf`,
  `archive/proto-screenshots/` — historical design artifacts.
- `docs/reference/pitofoldcode.txt` — a dump of deleted code the author kept.
- `archive/crashes/` — App Store crash logs from 1.1.1 (interesting archaeology for the
  threading bugs that led to the current single-threaded design).
- `tools/AtlasTool` (looks like the texture atlas builder) and `tools/eden2` (Java; `src/` holds the
  `List2.java`/`UploadMap2.java` server code, see [networking.md](networking.md)).
- `archive/Eden-mac/` — an experimental Mac project (`main.c` is a stub); unexplored.
- The upstream source tree (Ari Ronen's repository; provenance in
  `WORKING/feature-archaeology-2026-10-13/sweep-findings.md`) also carries `edenweb/` (the Java server),
  `jetty9/`, `javaworkspace/`, `flurry/`, `RunAndGun/`, `Eden_mac/`, `old website/` and `Classes *.zip`
  snapshots; none of them were imported into this repo.

## Guidance
- Don't "clean up" PVRT/CocosDenshion warnings; upstream fixes exist and local edits
  make diffing impossible.
- The hard iOS dependencies are OpenGL ES 1.1, NSFileHandle/NSData (FileManager), Texture2D
  (UIKit image loading), CocosDenshion (OpenAL), and UITouch handling — see
  [engine-vs-game.md](engine-vs-game.md) for the seam map. The native port covers every one of them
  with a seam or shim (`../port/CLAUDE.md`).

## Which "original" source tree this is
The original-Eden checkout used for archaeology is Ari Ronen's last commit `23121e6` (2015-03-09) plus four
2018 community commits (`aa224c6`, `4344e0e`, `b66d89e`, `40bcaae`: "Runs in Xcode", new burning
behaviours, a larger light range, an autosave, larger-iPhone fixes). Emod's tag `pristine-engine` is a
*different* community 2.1.1 build (`FLY_MODE=true`, `P_ZNEAR .012f`, the `app2`/`files2.edengame.net` URLs,
no `Autosave.mm`); 20 files differ. Anything sourced from those 2018 commits is a community change, not
Ari's. Details: `WORKING/feature-archaeology-2026-10-13/sweep-findings.md`.
