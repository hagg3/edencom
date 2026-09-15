# Third-Party & Vendored Code (Web Port)

Read [`../../docs/third-party.md`](../../docs/third-party.md) first — the PowerVR SDK,
CocosDenshion, GLU port, zlib/md5/hashmap, and everything else vendored into
`Classes/` is unchanged and still compiled as-is (except where seam-excluded per
[architecture-overview.md](architecture-overview.md)'s lever table). This file covers
what the web port adds on top.

## Toolchain dependency
`web/emsdk/` — Emscripten SDK, version 3.1.74 as of this writing, gitignored, the
port's equivalent of the root project's Xcode/iOS SDK dependency. See
[build-and-toolchain.md](build-and-toolchain.md).

## New vendored libraries (web-only)
- `src/shim/vendor/stb_image.h` — single-header PNG decoder, backing
  `src/seam/Texture2D_web.mm` (replaces `UIImage`/`CoreGraphics` decode, which has no
  web equivalent). See [resources-and-audio.md](resources-and-audio.md).
- `src/shim/vendor/stb_truetype.h` — vendored alongside `stb_image.h`; the text-raster
  use case it was meant for has disputed status (a "confirmed dead code" claim in the
  port's status notes is now doubted — see [ui.md](ui.md)'s world-name-in-menu gap).
- `-lidbfs.js` — an Emscripten-bundled JS library (not vendored source, but a
  load-bearing dependency), backing persistent `/documents` storage. See
  [save-load.md](save-load.md).

## New hand-written (not vendored) subsystems
- **ObjC runtime** (`src/shim/objc/`) — Emscripten ships none; see
  [objc-runtime.md](objc-runtime.md) for the ABI choice, dispatch model, and the
  ivar-layout fix.
- **GL ES 1.1 → WebGL2 shim** (`src/shim/gl/gl_es1_shim.{h,cpp}`) — see
  [gl-shim.md](gl-shim.md).
- **Foundation subset** (`src/shim/foundation/`) — roughly 18 classes reimplemented
  from scratch over stdio/IDBFS/localStorage: `NSObject`, `NSAutoreleasePool`,
  `NSString`, `NSArray`, `NSData`, `NSNumber`, `NSDate`, `NSFileHandle`,
  `NSFileManager`, `NSBundle`, `NSUserDefaults`, `NSThread`, `NSTimer`,
  `NSOperation`, `NSErrorException`, `CoreFoundation`, plus `md5_web.mm` (a real
  RFC-1321 MD5 replacing `md5.c`'s CommonCrypto plumbing — the digest has to be
  genuine, not stubbable, since it's used for preview-screenshot hashing) and
  `uikit_stubs.{h,mm}` (link-only stubs for the handful of CoreGraphics symbols the
  dead `JUST_TERRAIN_GEN`-only code path drags in). `NSURLConnection.h` exists only
  as a header-only link-time stub with no `.mm` — world-sharing networking is not
  implemented (see [networking.md](networking.md)). The directory's own
  `foundation-usage.md` documents grep-verified call sites per class — check it
  before assuming a class is unused.
- **Framework shim headers** (`src/shim/foundation/framework/`,
  `src/shim/gl/framework/`) — fake `<Foundation/Foundation.h>`, `<UIKit/UIKit.h>`,
  `<OpenGLES/ES1/gl.h>`, `<OpenGLES/ES1/glext.h>`, `<OpenGLES/EAGL.h>`,
  `<Availability.h>` for **angle-bracket includes only**. Quoted `"X.h"` includes in
  `Classes/*.mm` can't be redirected this way (quoted includes resolve relative to
  the including file first) — this is why seam `.mm` files reach into
  `../../../Classes/*.h` with explicit relative paths instead of relying on `-I`
  search order, and why the seam-vs-wrap decision test in
  [build-and-toolchain.md](build-and-toolchain.md) matters.
- `src/seam/pvrt_matrix_palette.cpp` — not a new library, but a `-Wl,--wrap=` shim
  over the **already-vendored** PVRT SDK's `CPVRTglesExt::LoadExtensions`, needed
  because that function's original implementation resolves to NULL off-Apple. See
  [gl-shim.md](gl-shim.md).
