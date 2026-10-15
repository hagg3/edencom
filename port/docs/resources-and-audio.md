# Resources & Audio (Port Layer)

> Written for the web build (retired 2026-10-13); the audio half is rewritten for the native backend.
> The web audio seam (Web Audio + `<audio>` channels, `.caf` decoders, media-session and
> visibility handling, the HTTP-served audio manifest) is at tag `web-port-final`
> ([web-port-archive](../../docs/web-port-archive.md)).

Read [`../../docs/resources-and-audio.md`](../../docs/resources-and-audio.md) first —
the `Resources` manager, texture atlas layout, and asset catalog are unchanged. This
file covers the two engine files that were seam-replaced (audio, textures) plus asset packaging.

## Audio: `native/src/seam/SimpleAudioEngine_native.mm`
Replaces `Classes/SimpleAudioEngine.mm` (the CocosDenshion/OpenAL façade). The header comment of
that file is the reference; in short:
- **Effects** are decoded once, cached by name, and mixed as `SDL_AudioStream`s bound to one playback
  device; `setEffectsVolume`/`getEffectsVolume` scale every voice. Per-sound distance attenuation is
  **not** implemented (the `playEffect` overload for it is dead code in `Resources.mm`).
- **Five channels** stream long files: `0` = music, `1` = ambience bed, `2` = water/lava proximity,
  `3` = portal proximity, `4` = treasure-cube proximity (docs/resources-and-audio.md's ambience
  write-up). `Classes/SimpleAudioEngine.h` exposes channel 0 as the unchanged
  `playBackgroundMusic`/`stopBackgroundMusic`/`get·setBackgroundMusicVolume`/`isBackgroundMusicPlaying`,
  and channels 1–4 as `playAmbience(layer,…)`/`stopAmbience`/`isAmbiencePlaying`/`setAmbienceFade`/
  `get·setAmbienceVolume` (`layer` = channel − 1; one settings slider drives all four). Every channel
  separates the settings-slider volume from the engine's per-frame crossfade: volume =
  `userVolume × clamp(engineFade, 0, 1)`. **Since 2026-10-05 music does too**:
  `setBackgroundMusicVolume` is the slider and `setBackgroundMusicFade` is `Resources::update`'s song
  crossfade (before that both wrote one knob and the slider moved the volume for one frame).
  `--audio-selftest` asserts the slider value survives 30 engine frames.
- **Two platform jobs sit behind one C wall**, `AppleAudio_native.h` — "decode any format" and "stream
  one long file": `AppleAudio_native.mm` (AudioToolbox/AVAudioPlayer) on macOS/iOS,
  `PortableAudio_native.cpp` (dr_mp3/dr_wav + a hand-written IMA4/LPCM `.caf` and AIFF decoder) on
  Linux/Windows. The library is 436 `.caf` (428 IMA4), 104 `.mp3`, 16 `.wav`, 1 `.aif`.
- **Bare file names**: the engine asks for `"explosion.caf"`; CMake links the `media/` tree flat beside
  the executable (the iOS bundle's layout), so no manifest is needed.
- **Door-sound trim: not on native.** The web seam played `door_open.mp3`/`door_close.mp3` at 0.5×
  (they are mastered hotter than the rest); the native seam has no equivalent, so doors are louder
  there. A parity gap, not a decision.
- `eden_native_audio_tick()` runs once per frame after the engine tick; harness modes other than
  `--audio-selftest` disable audio.

## Textures: `src/seam/Texture2D_web.mm`
Replaces `Classes/Texture2D.mm` (`UIImage`/`CoreGraphics` decode — no web
equivalent), following the "reuse the original `.h`, replace only the `.mm`" pattern
forced because `Texture2D.h` is quote-included by 8+ other engine headers. Decodes
PNGs via **stb_image** (`src/shim/vendor/stb_image.h`, see
[docs/third-party.md](../../docs/third-party.md)) instead of `CGBitmapContext`. An early
"X-mirror" bug suspected in this path was tracked down and proven to be a V-flip
elsewhere, not a flaw in the decode — this file carries no flip logic; don't
re-investigate that theory without a fresh measurement.

### The recolor pipeline (audit row 11 / A5 — implemented 2026-07-31)
`ManipulateImagePixelData()` used to be a stub returning null — pixel-level recolor
was deferred as "P2b (creature skin/mask recolor pipeline)" and then never picked up.
Four live call sites went through it, and all four drew nothing: the HUD paint icon
(`Resources::getPaintTex`), the painted build icons for flowers/gold cubes/portals/
doors (`getPaintedTex`), door blocks in the world (`Resources::getDoorTex`, called from `Terrain.mm`),
and creature skins (`getSkin`, `Model.mm`). The mechanism is worth remembering because
it is completely silent: a null `CGImageRef` reaches `new Texture2D(cgimage, …)`,
`initFromImage`'s `if (image == NULL) return;` leaves the GL `name` at 0, and every
subsequent draw binds "no texture" — no GL error, no warning, just missing art. That
is the invisible-paint-icon bug root-caused in pass 49.

It is now real, entirely in the seam/shim (no `Classes/` edit):

- **`CGImage` is a real struct** (`src/shim/foundation/uikit_stubs.h`): one
  tightly-packed RGBA8 buffer, row 0 = top, **straight (non-premultiplied) alpha**.
  Straight because stb_image never premultiplies, so every other texture in the port
  is straight too; CoreGraphics' `CGBitmapContext` does premultiply, but inside the
  recolored region the source art is alpha 255 (measured: 487 of 490 masked pixels on
  the real assets), so the two agree exactly where it matters.
- **`UIImage` is a real retain-counted owner of one `CGImage`.** Both ivars are POD
  on purpose — see [objc-runtime.md](objc-runtime.md) on why a C++ ivar in a shim
  `@implementation` is never constructed or destroyed (Linux/Windows).
- **`Texture2D::initFromImage` carries the upload pipeline** (POT rounding, canvas
  placement, pixel-format packing) and `initFromPath` decodes to a `CGImage` and calls
  it — which is the engine's own shape, and means the PNG path and the recolor path
  share one code path instead of two.
- **`initFromPath` now runs the engine's `storeImage` block**, filling
  `Resources.mm`'s `storedPaint`/`storedPaintMask`/`storedSkins[5][2]`/… globals. This
  is a **positional** scheme, not a lookup: `Resources::loadResources` zeroes
  `storedSkinCounter` immediately before loading the 15 creature-skin PNGs (every
  third — the Rage variant — is deliberately not stored, giving 10) and
  `storedMaskCounter` before the 10 MASK PNGs, and the slot is decided by how many
  textures have loaded since. **Reorder texture loads in `Resources.mm` and creatures
  wear each other's skins.** The counters advance before the decode and regardless of
  whether it succeeds, so a missing asset must never skip them.
- **Ownership**: the ~14 images the recolor reads back from are wrapped in a `UIImage`
  and kept forever (~5.4 MB, dominated by the 20 256×256 skin/mask pairs); the other
  ~110 are freed as soon as their pixels are in GL. The load path uses
  `-[UIImage initWithCGImage:]`, **not** `+imageWithCGImage:` — `loadResources` runs
  during `World` construction, before any frame and therefore before any real
  autorelease pool exists, and the shim's fallback root pool is never drained. Using
  the autoreleased form there would keep every decoded texture resident for the life
  of the process. The recolor path itself is per-frame, so it correctly uses the
  autoreleased form and audit row A2's per-frame pool disposes of it.

Native `--recolor-selftest` guards all of this (asset capture, decode sizes, non-zero GL names,
per-colour cache behaviour, and the two positional counters landing on exactly 10; W.2, it
replaced the web `headless-recolor-test.js`). The probe it drives (`eden_debug_recolor_state`) is
`EDEN_DIAGNOSTICS`-gated like every other probe in `DebugState_web.mm`.

`ManipulateImagePixelData2` (the maskless variant) is implemented too, faithfully
including its quirk of deriving luminance from the blue channel alone. Every call site
of it is commented out in `Resources.mm`; it is implemented rather than stubbed so the
symbol does not lie about what it does.

## Asset packaging
`native/CMakeLists.txt` ("The bundle: a FLAT directory of assets beside the executable") links every
asset from `media/` (textures, UI, HUD art, models, sounds, music, fonts) flat beside the executable
— the layout Xcode's Copy-Bundle-Resources phase produced — and the iOS target copies the same set
into the `.app`. The bundled map is `Eden.emod`. `screens/` (marketing art) is not shipped.

## Memory note
Decoded textures are RGBA8; since P.15 only textures that ask for mips get a chain (gl-shim.md
"Texture uploads"). No compressed texture formats are used — a plausible future memory win, not
implemented (audit row C3).

On top of the GL-side cost, the recolor pipeline keeps ~5.4 MB of **decoded source
pixels** resident: 10 creature skins + 10 masks at 256×256 RGBA8 are ~2.6 MB each, and
the paint/door/icon pairs add ~0.2 MB. That is not removable — `getSkin` recolors on
demand at runtime whenever its cache misses, so the sources have to stay. The original
iOS build paid exactly the same cost.
