# Resources & Audio

## Purpose
`Resources` is the asset manager: texture atlases, UI textures, creature skins, and
the entire audio layer (sound effects, ambience, music, creature voices).

## Important files
- `Classes/Resources.mm/.h` (1590 lines) — the manager, `Resources::getResources`
  singleton.
- `Classes/Texture2D.mm` — texture loading (PNG/PVR paths) + string rasterization.
- Audio stack: `Classes/SimpleAudioEngine.mm` (C++ wrapper) over
  `SimpleAudioEngine_objc` / `CocosDenshion` / `CDAudioManager` (OpenAL) — the
  CocosDenshion library, vendored.
- Sound assets: the hundreds of `.caf`/`.m4a` files referenced in the Xcode project
  (block break/build per material ×4 variants, footsteps, creature voice sets per
  species×emotion×5, ambience loops, UI clicks, music `Eden_1..6.m4a`).

## Textures
- `atlas` — the opaque block atlas: a vertical strip of 32 tiles; the mesher stores
  a tile index and the texture matrix scales v by 1/32 ([rendering.md](rendering.md)).
  `getBlockTexShort(texId)` returns the tile origin/height.
- `atlas2` — the transparent/animated atlas (water/lava frames, glass, leaves…);
  rows are animation frames advanced by the render pass.
- `textures` / `menutextures` vectors — indexed by `ICO_*` enums (sky boxes, swirl,
  flower sheet, spheremap, HUD icons…). Game vs. menu sets are loaded/unloaded on
  world enter/exit (`loadGameAssets`/`unloadGameAssets`,
  `loadMenuTextures`/`unloadMenuTextures`) to fit early-device memory.
- `getDoorTex(color)`, `getPaintTex`, `getPaintedTex(type,color)`,
  `getSkin(model,color,state)` — lazily-built colored variants.

## Audio
- `playSound(soundId)` — `NUM_SOUNDS 73` effect groups (`enum SOUND_TYPES`,
  `Resources.h`), each with random variants, preloaded via
  `[SimpleAudioEngine preloadEffect:]`. `S_MENU_BUTTON_PRESS`/`_RELEASE`,
  `S_SKY_CHANGE_DARK_TO_LIGHT`/`_LIGHT_TO_DARK`/`S_SKY_PAINTING`,
  `S_WARP_HOME_ACTIVATED`/`_LOCATION_SET`, `S_WORLD_SAVED`, `S_EXIT_WORLD` (53-61)
  were appended to wire up `media/new_sound/*.mp3` files that shipped in the repo
  but were never referenced from any call site — call sites: `Hud.mm`'s HUD
  menu-icon press/release (`rcam`/`rhome`/`rsave`/`rexit`/`rmenu`), its
  `handlePickMenu` Save button and `delayedaction` 5/6 (exit-to-menu / warp-home)
  branches, `asetHome()`, and `TerrainGen2.mm`'s `paintSky()` (direction inferred
  from whether `final_skycolor` was/becomes `colorTable[54]`, the night/black
  entry). `S_SWITCH_TOGGLE_ON`/`_OFF` (62-63, single-variation each) were added
  later, sourced from `menu_button_press_01.mp3`/`menu_button_release_01.mp3` —
  those two files were pulled OUT of `S_MENU_BUTTON_PRESS`/`_RELEASE`'s random-
  variation pool (now 4 variations, `_02` through `_05`) because variation 01
  audibly stands out from the rest; it's reserved for the web port's settings-
  panel boolean switches (`eden_play_switch_toggle_sound`, `Settings_web.mm`) and
  no longer turns up on an ordinary button click. Appending/reorganizing sound
  IDs freely is fine — unlike block types/`colorTable`, sound IDs aren't part of
  any on-disk struct, so the format-freeze rule doesn't apply to them.
  `S_ICE_LOOP_SLOW`/`_MEDIUM`/`_FAST` + `S_ICE_TURN`, `S_JOYSTICK_BEGIN`/`_RELEASE`,
  `S_JUMP_BUTTON_PRESS`/`_RELEASE`, and `S_MODE_SELECTION` (64-72) wire up the last
  batch of previously-unused `media/sound/game/*.caf` + `media/new_sound/*.mp3` files.
  Call sites: `Player::move`'s ice-sliding block picks a loop by current speed (`mag`,
  thresholds at 4/8 — a heuristic off `max_walk_speed`'s ~5.85 baseline, since ice
  sliding has no speed cap) via the new `playLoopedSound` (a real `bLoop=TRUE` voice,
  unlike `playSound`'s one-shot-only `playEffect` call) and restarts it on a bucket
  change, held for at least .4s (`iceBucketHoldTimer`) before another switch is allowed:
  `mag` is re-evaluated every frame and naturally hovers right on a threshold during
  normal sliding, and `SimpleAudioEngine_web.mm`'s `playEffect` fetches+decodes
  asynchronously before calling `start()`, so a same-frame stop+restart cancels the new
  voice before its fetch resolves (its id is already gone from `A.voices` by the time
  the promise lands) — without the hold, only whichever bucket `mag` stayed in longest
  was ever actually heard, and an occasional switch that *did* survive read as a quiet,
  cut-off blip. `S_ICE_TURN` fires on the existing 45°-cross-product wedge/ramp auto-turn
  detection, which already only fires once per real turn. `Joystick.mm`'s drag-begin
  (first frame a touch claims the joystick, `touches[i].inuse==0`) and release
  (`touches[i].down==M_RELEASE`); `Hud.mm`'s jump button needed a prev-frame latch
  (`wasJumpPressed`) since `m_jump` is re-derived from touches every frame (level, not
  edge — see `web/CLAUDE.md`'s "any `hud->` input flag" note) and would otherwise fire
  every frame it's held. `S_MODE_SELECTION` plays from `eden_menu_create_world`
  (`web/src/seam/Menu_web.mm`) on a successful world creation.
- **Touch-controls sounds** (joystick begin/release, jump-button press/release) are
  gated on two independent things: the `touch_controls_sound` setting
  (`Settings_web.mm`, `bool touchControlsSoundEnabled` in `Classes/Player.mm`) — off by
  default — and, regardless of that setting, `UITouch`'s `isRealTouch`
  (`web/src/shim/foundation/uikit_stubs.h/.mm`), which is `FALSE` for a synthetic touch
  Input_web.mm manufactures to drive a HUD button from the keyboard or mouse (its
  identity is negative; only a real `Touch.identifier` from the browser is `>=0`) — so
  e.g. Space-to-jump can never make a sound even with the setting on. Keeping the
  identity-numbering scheme itself inside the shim (`isRealTouch` is a yes/no method,
  not a raw identity comparison) is what lets `Classes/Hud.mm`/`Joystick.mm` ask the
  question without knowing a platform detail, per `web/CLAUDE.md` rule 2.
- **Bugs fixed alongside the above** (all previously-shipped, previously-silent):
  `S_WARP_HOME_ACTIVATED` was called *after* `Terrain::warpToHome()`, which calls
  `loadTerrain()`, which sets the file-scope `firstframe=TRUE` for the rest of that
  frame — and `Resources::playSound`'s `!firstframe` guard silently drops everything
  while it's set, so the sound never played; fixed by playing it first (`Hud.mm`).
  `S_DOOR_CLOSED` (`door_close.mp3`, already mapped) had its call site commented out in
  `Terrain.mm`'s door-animation code, symmetric with the (working) `S_DOOR_OPEN` call
  right above it — uncommented. `S_EXPLODE` and `S_ATTEMPT_FIRE` both had more files
  listed in `soundFiles[]` than `sfxNumVariations[]` allowed (1 each), so only their
  first variation ever played despite the others already existing on disk —
  `S_ATTEMPT_FIRE` now also cycles `attempt_fire.caf`/`attempt_fire_v2.caf` (3
  variations total) and `S_EXPLODE` now also cycles `explosion_1_v2..4_v2.caf` (8
  variations total, `MAX_VARIATIONS2` raised from 6 to 8 to fit — just an array width,
  not an on-disk format).
- **More bugs found from live play feedback on the above** (all previously-shipped):
  - Ambience channels kept playing into the menu after `World::exitToMenu()`: the
    fade-stepping in `Resources::update` ran inside `if(playmusic)` only, never gated on
    `game_mode==GAME_MODE_PLAY`, and `soundEventBed`/`Proximity`/`PortalProximity`/
    `TreasureProximity` all early-return (without stopping anything) once `game_mode`
    leaves `PLAY` — so whatever was last playing (night ambience was the one that
    surfaced it) just kept looping since nothing ever called `stopAmbience` again.
    Fixed by forcing every target to 0 and stopping+clearing a channel once it's
    actually faded silent, whenever `game_mode!=GAME_MODE_PLAY`.
  - The door open/close sounds fired spuriously (a burst of them on world load, on
    chunk streaming, and on any nearby block edit) because `TerrainChunk.mm`'s mesher
    rebuilds each door's `StaticObject` from scratch on every remesh — not just when the
    door itself changes — and used to hardcode `ani=0`/`rot=M_PI/2` regardless of the
    player's actual position, a value distinct from both real states (-1 open, 1
    closed), so `Terrain::render`'s `prev_ani!=ani` transition check fired a sound every
    single remesh. Fixed by seeding `ani`/`rot` to match `render()`'s own distance test
    at creation time instead of a fixed sentinel.
  - Portal proximity ambience (and, more seriously, `Portal::enterPortal` itself —
    actual teleporting) silently stopped finding portals whose chunk hadn't been
    remeshed since the *last* `Terrain::unloadTerrain(FALSE)` call (`warpToHome`, and a
    few other internal reload paths) — `unloadTerrain` cleared `portals->
    removeAllPortals()` unconditionally, but `Portal::addPortal` only runs from a mesh
    rebuild (`TerrainChunk.mm`), which most portals never get again unless their chunk
    is dirtied or streams back in fresh. Moved the registry wipe (and the fireworks
    one alongside it) to only happen when `exitToMenu==TRUE`, i.e. when the mesh cache
    itself (`troot`) is also being discarded — see `docs/world-and-terrain.md`.
  - The touch-only sounds below additionally needed a settings toggle and a real
    touch-vs-synthetic check — see the touch-controls paragraph.
- Burn loop management: `startedBurn(length)/endBurnId/endBurn` reference-count the
  fire loop so 50 burning blocks don't play 50 loops.
- `soundEventBed(action[, location])`/`soundEventProximity(action[, location])` —
  positional triggers (distance attenuation), one call per independent ambience layer
  (see below). `soundEvent` (singular) no longer exists as of the web port's
  audio-channel split — every call site names the layer it means.
- `voSound(action, creatureType, location)` — creature voice lines
  (`VO_*` actions × 7 species × 5 variants).
- Ambience: `NUM_AMBIENT 17` environment loops (`AMBIENT_UNDERWATER`, `_CAVE`,
  `_GRASSLANDS`, `_PYRAMID`, `_NIGHT`…), split into **four** independent, simultaneously-
  audible layers (each with its own crossfade state, stepped every frame in
  `update(etime)`):
  - **Bed layer** (`soundEventBed`, `Player.mm`): exactly one of
    {underwater, sky-high, cave, open, the 11 biome ambiences} at a time, chosen by the
    same underwater>sky-high>cave>biome priority chain as before, crossfading between
    changes. `AMBIENT_NIGHT` (already had a file, `night_time_ambience.mp3`, but no call
    site) now substitutes for `AMBIENT_OPEN` whenever `Terrain::final_skycolor` equals
    the night sky color (`colorTable[54]`, the same test `Terrain::render` uses to dim
    the world lights for lamps to matter) — so it plays instead of, never alongside,
    the plain overworld ambience, and every biome/cave/underwater/sky-high ambience
    still takes priority over it exactly as they do over `AMBIENT_OPEN`.
  - **Three of the bed layer's filenames never resolved** (found 2026-09-05, Phase N Stage 2,
    and dead on **both** targets — the web manifest is keyed by basename, so a name with no file
    behind it just resolves to null and the bed plays silence). `AMBIENT_GRASSLANDS` asked for
    `ambience_open.mp3`, which has never existed; the shipped file is
    `media/new_sound/grasslands_ambience.mp3`, and `ambience_open` is the name of the `.wav` bed.
    That is the bed for ordinary daytime open terrain, i.e. the one a player hears most. Fixed in
    the `ambientFiles[]` table. The other two were broken the other way round — the table's
    spelling was the sensible one — and were fixed by renaming the ASSET:
    `reverlands_ambience.mp3` → `riverlands_ambience.mp3`, and
    `" mountain_grass_badlands_ambience.mp3"` (leading space) → without it. All 17 entries in
    `ambientFiles[]` now resolve.
  - **Water/lava proximity layer** (`soundEventProximity`, `Player.mm`): water/lava
    adjacency only (`AMBIENT_RIVER`/`AMBIENT_LAVA`/`AMBIENT_NONE`), independent of the
    bed layer — it no longer preempts (or gets preempted by) the bed layer or music, so
    e.g. standing near lava inside a normal biome plays both the biome bed ambience and
    the lava proximity ambience together. `Player.mm` explicitly calls
    `soundEventProximity(AMBIENT_NONE, pos)` when no water/lava block is nearby, so this
    layer fades out on its own instead of relying on the bed chain's old mutual
    exclusion.
  - **Portal proximity layer** (`soundEventPortalProximity(active, location)`,
    `Player.mm`): fades in `ambience_nearby_portal.mp3` within 15 blocks of the nearest
    portal. Unlike the water/lava layer this isn't part of the `AMBIENT_*` table (a
    single always-the-same file, on/off + distance rather than an enum lookup) — driven
    by `Portal::nearestPortal(pos,&outPos)` (Portal.h/.mm), a linear scan of the portal
    registry (portals are rare and already tracked there for teleport pairing).
  - **Treasure-cube proximity layer** (`soundEventTreasureProximity(active, location)`,
    `Player.mm`): same shape as the portal layer, `ambience_nearby_treasure_cube.mp3`
    within 15 blocks of the nearest `TYPE_GOLDEN_CUBE`. Treasure cubes are plain voxels
    with no position registry, so `Player.mm` runs a flat 31×31×3 box scan around the
    player to find the true nearest one — throttled to 4×/sec (a static timer) since the
    ambience fade is slow enough that this cadence reads as continuous, and doing it
    every frame would be needlessly expensive when nothing is nearby.
  - All four layers are gated on `playmusic`/`GAME_MODE_PLAY` exactly as the old single
    `soundEvent` was, and no longer gated on `songisplaying` — music and ambience are
    fully independent channels now (see `Classes/SimpleAudioEngine.h`'s `playAmbience`/
    `setAmbienceFade`/`setAmbienceVolume` family, web-only; see
    `web/docs/resources-and-audio.md`). `playAmbience`'s `layer` parameter: 0=bed,
    1=water/lava proximity, 2=portal proximity, 3=treasure-cube proximity.
- Music: `playMenuTune/stopMenuTune` — title screen rotates randomly (no immediate
  repeat) through `NUM_TITLE_SONGS 2` tracks (`titleSongFiles`: `Eden_title.mp3`,
  `Eden_title_2011.mp3`),
  cued non-looping and advanced to the next random track by a per-frame
  `GAME_MODE_MENU` poll of `isBackgroundMusicPlaying()` in `update(etime)`; in-game
  music tracks (`NUM_SONGS 6`, `songFiles`) rotate the same way via the same engine
  but on a `TIME_BETWEEN_SONGS` cadence instead of track-end.
- User toggles `playmusic`/`playsound` come from the settings menu.

## Lifecycle
Constructed first thing in `World::World()` (before Terrain, because loading screens
need textures/sounds). Menu assets live while in menu; game assets while playing;
both swapped in `World::loadWorld`/`exitToMenu`.

## Common pitfalls
- Texture loads must happen with the GL context current (main thread).
- The `ICO_*` indices are positional — adding a texture mid-list shifts everything
  after it; append only.
- Preloading all sounds at startup is why first launch is slow in the simulator.
- `Sound.h/.m` (a distinct, older wrapper) coexists with CocosDenshion; `Resources`
  is the only sanctioned entry point — don't call the engines directly.

## Safe vs. risky to modify
- **Safe:** adding sounds/textures via the existing tables, tuning ambience rules.
- **Caution:** atlas layout (coupled to `blockTypeFaces` and the 1/32 texture-matrix
  scale), load/unload pairing (double-frees on the menu↔game boundary are a classic
  crash here).
