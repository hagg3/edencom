# Resources & Audio (Web Port)

Read [`../../docs/resources-and-audio.md`](../../docs/resources-and-audio.md) first —
the `Resources` manager, texture atlas layout, and asset catalog are unchanged. This
file covers the two engine files that had no web equivalent and were seam-replaced,
plus the asset-packaging split.

## Audio: `src/seam/SimpleAudioEngine_web.mm`
Replaces `Classes/SimpleAudioEngine.mm` (the CocosDenshion/OpenAL façade — no OpenAL
on web). Effects go through **Web Audio**; music and all four ambience layers each
stream through their own `<audio>` element ("channel"). Sound assets need hand-written
**IMA4/LPCM `.caf` decoders** since there's no CoreAudio to lean on.
`setEffectsVolume`/`getEffectsVolume` (previously entirely unimplemented — the real
file defining them was excluded) route all effect voices through one shared
`GainNode`. Per-sound distance attenuation is **not** implemented — the relevant
`playEffect` overload is dead/commented code in `Resources.mm`, and implementing it
would require an engine-side change, which is off-limits (the four ambience layers get
their distance fade a different way — see the ambience ID pair below).

**Door sounds too loud (fixed 2026-07-31):** `door_open.mp3`/`door_close.mp3`
(`Classes/Terrain.mm`'s door-animation code, `S_DOOR_OPEN`/`S_DOOR_CLOSED`) are
mastered noticeably hotter than the rest of the (mostly `.caf`) effect library, and
`SimpleAudioEngine::playEffect(filePath, loop)` takes no gain argument to correct it
per-call — a years-old complaint predating the port. Fixed at the seam layer, not the
asset: `eden_audio_play_effect` (`src/seam/SimpleAudioEngine_web.mm`) matches those two
filenames and inserts a one-off trim `GainNode` (0.5×) between the voice and
`effectsGain` only for them. Any other effect that turns out to be mismastered should
get the same filename-keyed trim rather than a broader API change.

**Five independent playback channels**, each a `{audio, name, wanted, userVolume,
engineFade}` record in `A.channels` (JS side, `eden_audio_js_init`'s `EM_JS` block):
`0`=music, `1`=ambience bed, `2`=ambience water/lava proximity, `3`=ambience portal
proximity, `4`=ambience treasure-cube proximity (see `../../docs/resources-and-
audio.md`'s ambience-layer writeup for what each means). Generic exports —
`eden_audio_play_channel/stop_channel/is_channel_playing/set_channel_user_volume/
set_channel_fade` — replace the old music-only `eden_audio_play_music` family; the
`Classes/SimpleAudioEngine.h` façade exposes channel 0 as the unchanged
`playBackgroundMusic`/`stopBackgroundMusic`/`get·setBackgroundMusicVolume`/
`isBackgroundMusicPlaying`, and channels 1-4 as a new `playAmbience(layer,...)`/
`stopAmbience`/`isAmbiencePlaying`/`setAmbienceFade`/`get·setAmbienceVolume` family
(`layer` = channel − 1, i.e. 0=bed, 1=water/lava proximity, 2=portal proximity,
3=treasure-cube proximity; `getAmbienceVolume`/`setAmbienceVolume` take no layer — one
settings slider drives all four). Only the four ambience channels separate the
settings-slider volume (`userVolume`) from the engine's per-frame crossfade
(`engineFade`) — `audio.volume = userVolume * clamp(engineFade,0,1)`; the music
channel keeps its original single-volume behavior (`setBackgroundMusicVolume` is
called by both the settings slider and `Resources::update`'s song-crossfade math,
last write wins, unchanged from before this split) since nothing asked for that to
change.

**Hardware media keys and backgrounding** (also in `eden_audio_js_init`): a
no-op `navigator.mediaSession.setActionHandler` for `play`/`pause`/`stop`/
`seekbackward`/`seekforward`/`previoustrack`/`nexttrack` stops Bluetooth/OS media
keys from pausing or resuming game audio. A `visibilitychange` listener pauses every
channel's `<audio>` (without clearing its "wanted" flag) and suspends the effects
`AudioContext` when the tab is hidden, and resumes both when it's visible again —
this is what stops iOS from playing game sound like a backgrounded music app when the
tab loses focus or the screen locks. Both are guarded the same headless-safety way as
everything else here (below).

**Headless-safety rule, found the hard way**: `Resources()` — and therefore the audio
engine — constructs unconditionally in `World::World()` at app startup, so any
audio-init JS that references `window`/`document` unconditionally will throw
`ReferenceError` under `node eden.js` (no DOM) and abort before the tick loop even
starts. This regressed once and silently broke the headless fast-iteration loop for
several passes before being caught. Guard any such reference with
`typeof window !== 'undefined'`, not just interaction-gated code paths.

## Textures: `src/seam/Texture2D_web.mm`
Replaces `Classes/Texture2D.mm` (`UIImage`/`CoreGraphics` decode — no web
equivalent), following the "reuse the original `.h`, replace only the `.mm`" pattern
forced because `Texture2D.h` is quote-included by 8+ other engine headers. Decodes
PNGs via **stb_image** (`src/shim/vendor/stb_image.h`, see
[third-party.md](third-party.md)) instead of `CGBitmapContext`. An early
"X-mirror" bug suspected in this path was tracked down and proven to be a V-flip
elsewhere, not a flaw in the decode — this file carries no flip logic; don't
re-investigate that theory without a fresh measurement.

### The recolor pipeline (audit row 11 / A5 — implemented 2026-07-31)
`ManipulateImagePixelData()` used to be a stub returning null — pixel-level recolor
was deferred as "P2b (creature skin/mask recolor pipeline)" and then never picked up.
Four live call sites went through it, and all four drew nothing: the HUD paint icon
(`Resources::getPaintTex`), the painted build icons for flowers/gold cubes/portals/
doors (`getPaintedTex`), door blocks in the world (`getDoorTex`, `Terrain.mm:2850`),
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
  on purpose — see the `NSUserDefaults.mm` write-up on why a C++ ivar in an
  `@implementation` is never constructed in this port.
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
  of the tab. The recolor path itself is per-frame, so it correctly uses the
  autoreleased form and audit row A2's per-frame pool disposes of it.

`tools/headless-recolor-test.js` guards all of this (asset capture, decode sizes,
non-zero GL names, per-colour cache behaviour, and the two positional counters landing
on exactly 10). Run it against `build-st`; the probe it drives
(`eden_debug_recolor_state`) is `EDEN_DIAGNOSTICS`-gated like every other probe in
`DebugState_web.mm`.

`ManipulateImagePixelData2` (the maskless variant) is implemented too, faithfully
including its quirk of deriving luminance from the blue channel alone. Every call site
of it is commented out in `Resources.mm`; it is implemented rather than stubbed so the
symbol does not lie about what it does.

## Asset packaging split
- **Textures/UI/HUD art** (`media/textures`, `menu`, `menu_text`, `ui`, `icons`,
  `ipad_menu`, plus a few individually named root-level PNGs like `sky_box.png`) —
  `--preload-file`'d into `/bundle/media/...`, preserving subdirectories.
- **Audio** (~45 MB of `.caf`/`.mp3`/`.wav` under `../media`) — deliberately **not**
  preloaded (would double the initial download). Served over plain HTTP via a
  `public/audio -> ../media` symlink, plus a generated
  `public/audio-manifest.json` mapping bare filenames (the iOS bundle is flat) to
  their real `media/` subdirectory.
- **`media/{models,music,new_sound,sound,screens}`** are excluded from the texture
  preload set — audio is HTTP-fetched separately (above), `screens/` is unused
  marketing art, and `models/` (creature PODs + skins) is preloaded flat separately
  — see [entities-and-creatures.md](entities-and-creatures.md).

## Memory note
Decoded textures are RGBA8 with mipmaps; no compressed texture formats are used — a
plausible future memory win, not implemented (audit row C3).

On top of the GL-side cost, the recolor pipeline keeps ~5.4 MB of **decoded source
pixels** resident: 10 creature skins + 10 masks at 256×256 RGBA8 are ~2.6 MB each, and
the paint/door/icon pairs add ~0.2 MB. That is not removable — `getSkin` recolors on
demand at runtime whenever its cache misses, so the sources have to stay. The original
iOS build paid exactly the same cost.
