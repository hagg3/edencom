# Third-Party & Vendored Code

Roughly half the files in `Classes/` are vendored libraries. Treat them as read-only
dependencies; none have been meaningfully forked except where noted.

| Component | Files | Role | Notes |
|---|---|---|---|
| **PowerVR SDK (PVRT tools)** | `PVRT*.cpp/.h`, `PVRShell*` (~40 files) | POD model loading/animation (`CPVRTModelPOD` — creatures), math types (`PVRTVec/Mat/Quaternion`), PVR texture support, tri-stripping | Only the model/math/texture parts are actually used; PVRShell/Print3D are along for the ride. Imagination Technologies license headers apply. |
| **CocosDenshion** | `CocosDenshion.*`, `CDAudioManager.*`, `CDOpenALSupport.*`, `CDConfig.h`, `SimpleAudioEngine*` | OpenAL audio engine (effects) + AVAudioPlayer (music) | Both the ObjC original and the cocos2d-x C++ wrapper (`SimpleAudioEngine.mm`, namespace `CocosDenshion::`) are present; `Resources.mm` uses both. |
| **cocos2d fragments** | `CCDirector/CCCamera/CCScheduler/CCConfiguration.cpp`, `ccFPSImages.c` | Support code the C++ SimpleAudioEngine port dragged in | Not a real cocos2d integration — the game has its own loop. |
| **Texture2D** | `Texture2D.mm/.h`, `OpenGL_Internal.h` | Apple's classic texture + string-texture class | Extended with `drawSky` etc. — this one *is* locally modified. |
| **MESA GLU port** | `glu.h`, `glue.c`, `project.c`, `registry.c`, `error.c`, `gluos.h`, `gluint.h`, root `la-map.c` | `gluPerspective`, `gluUnProject` (block picking), `gluLookAt` | SGI Free Software License B headers. |
| **zlib helper** | `zpipe.c/.h` | Deflate/inflate streams | Only user was the disabled `FileArchive` + `gzipInflate`. |
| **md5** | `md5.c/.h` | Preview-screenshot hash | Public-domain RSA-derivative implementation. |
| **hashmap** | `hashmap.mm/.h` | Int-keyed open-addressing hashmap | Generic; used for save-file directories. Small enough to be considered first-party by now. |
| **Appirater** | `Appirater.mm/.h` | "Rate this app" prompt | Called from the app delegate. |
| **Flurry** | `flurry/` (binary lib) + `[Flurry startSession:...]` in the delegate | Analytics | Session key is hard-coded in `EdenAppDelegate.mm:33`; consider removing in derivatives. |
| **TestFlight** | `Eden/libTestFlight.a` | Beta crash reporting | All call sites commented out; the .a remains linked in the project. |

## Repo-root miscellany (not compiled)
- `MROB.txt`, `Eden_file_format.txt` — file-format documentation (see
  [eden-file-format.md](eden-file-format.md)).
- `EdenDesignDoc.pdf`, `sound_design_draft.pdf`, `proto screenshots/`,
  `old website/` — historical design artifacts.
- `pitofoldcode.txt` — a dump of deleted code the author kept.
- `crashes/` — App Store crash logs from 1.1.1 (interesting archaeology for the
  threading bugs that led to the current single-threaded design).
- `javaworkspace/`, `edenweb/`, `jetty9/` — server + tooling (see
  [networking.md](networking.md)); `javaworkspace/AtlasTool` looks like the texture
  atlas builder; `javaworkspace/eden2` unexplored.
- `RunAndGun/`, `Eden_mac/`, `Eden-mac/` — unrelated/experimental projects
  (`Eden-mac/main.c` is a stub); unexplored.
- `Classes *.zip` — snapshot backups of the Classes directory; ignore.

## Guidance
- Don't "clean up" PVRT/CocosDenshion warnings; upstream fixes exist and local edits
  make diffing impossible.
- If porting off iOS: the hard dependencies are OpenGL ES 1.1, NSFileHandle/NSData
  (FileManager), Texture2D (UIKit image loading), CocosDenshion (OpenAL), and
  UITouch handling — see [engine-vs-game.md](engine-vs-game.md) for the seam map.
