# Beta 0.5
This beta is mostly about polish and robustness: a keyboard-friendly interface, a hotbar, desktop window handling, and a lot of hardening so a bad save, a full disk or a hostile mod can't take your world down. Everything below is new since Beta 0.4. Bug reports and suggestions are welcome on the [Discord server](https://discord.gg/SgEQfurMmj).

## Interface
- **Hotbar:** a nine-slot strip along the bottom of the desktop HUD. Keys 1-9 and the mouse wheel pick a slot, the current one is raised, and each slot shows the block's icon. Each world remembers its own hotbar. On touch devices it is a Gameplay setting (off by default) and the slots are tappable. Mods can fill slots.
- **Keyboard and mouse navigation in every menu:** Tab and Shift-Tab, arrow keys, Return and Space work on all screens, with a visible focus ring and a hover highlight. Return presses the screen's main button (Play, Join, Save, Download, Resume). None of this appears on touch devices.
- **Leaving a form with unsaved changes asks first** (World Properties, add/edit server): *Discard changes?*
- **Phone layout fixes:** overlapping controls in New World, clipped values in World Properties, Get Worlds detail text sitting under Download, icons over labels in the pause menu, the titlebar button touching the panel edge, and the console strip covering the gear.
- **Cleaner text:** sentence case everywhere, red *danger* styling on Delete and Change terrain, and clearer hint text. The version tag moved next to the gear on the HUD. The jump button is hidden when there is no touch chrome. The mode buttons on the right take clicks when the pointer is free.
- The server list shows *Profile: <name>* and disables Join with a hint until you have set a name.

## Desktop
- **Fullscreen** (Settings › Video, or F11). Window size, position and fullscreen state are remembered between launches, and the window has a minimum size.
- **Quit button** on the home screen (desktop only).
- **An unfocused window now sleeps** instead of using a full CPU core (about 1 % idle on a Mac, down from about 90 %). Multiplayer sessions stay connected.

## Saving and worlds
- **A failed save is now visible and loses nothing.** If the disk is full or the write fails you get a message, *Save & Quit* is blocked until it works, and saves back off and retry.
- **A bad world file no longer blocks the next load**, and the reason is shown on the status bar.
- **Corrupt or hostile worlds are handled:** NaN or huge spawn, home or creature positions, oversized records and images, and damaged `.emod` tails are clamped or refused instead of crashing. This fixes a rare crash when loading some worlds (a creature spawn index could wrap negative).
- **Safer small files:** settings, server list, mod list and hotbar are written atomically, so a crash or full disk mid-write can't leave them half-written.
- **Low-memory warnings** (iOS and Android) now save your world and free audio before the system steps in.
- **Downloads have size limits** (worlds 4 GB, mods 32 MB) and need some free disk space.
- Switching the day/night cycle on or off streams in over a few frames instead of stalling.

## Mods
- **A mod script can no longer hang the game** (hostile Lua calls are capped).
- Entering a world tells you when mods it uses are off or missing.
- Packs can contribute hotbar slots.

## Fixes
- **Linux builds:** fixed a compile error from Lua's `next` macro clashing with libstdc++.
- Several threading races between loading a world and the main loop are fixed. Input is ignored while a world loads.
- Mod commands now declare whether they are multiplayer-safe, and unknown ones are refused.
- The weather and atmosphere code now agrees on exactly what *Off* renders (the same frame as before the feature existed).

## Under the hood
- One list of tests for every CI platform; compiler warnings are on and the uninitialised reads they found are fixed; a new thread-sanitiser CI job; settings are applied from a data table; menu Back routing moved into the engine; leftover web-port code was deleted.
- Mod-pack fixtures moved to `port/tools/fixtures/packs/`.

# Beta 0.4
This is a fairly feature-heavy beta. It includes experimental modding support; lots of new features to enhance the in-game atmosphere; and an overhaul to settings.
There are also experimental 'srclike' movement options for anyone who wishes to bhop or surf in an Eden world.
 [Discord server](https://discord.gg/SgEQfurMmj).

## Modding (new)
- **Block registry:** worlds can now hold custom blocks from mod packs. Worlds contain a palette of blocks (`namespace:name`); missing textures show for blocks that are not installed and/or loaded. Stock block numbers are unchanged.
- **Data-driven content packs:** block properties and textures now load from `pack.json` data files (the stock blocks are the built-in `eden` pack). They are stored in `Documents/Packs/`.
- **Mods manager:** enable, disable and install packs, with dependency and conflict checks.
- **Lua mod scripts:** packs can include sandboxed Lua 5.4 scripts that use the `eden.*` API. See `docs/scripting.md`.
- **Wonder Blocks example pack**, contains 23 blocks, many of which have custom features.
- **Block palette import/export** (`BPL1`) for moving custom blocks between worlds.

## Worlds
- **World folders:** each world now lives in its own folder (`Worlds/<id>/world.emod`) with its settings and thumbnail beside it. Existing worlds are adopted automatically.
- **Optional day/night cycle** (Settings › Experiments, off by default): gradual sunrise and sunset colours, a starry night sky, pixel-art sun and moon, and terrain that dims smoothly.

## Atmosphere 
- **Weather:** rain, overcast, snow, storms (with lightning and thunder), and mist.
- **Drifting pixel clouds**, **ambient particles**, and water and light polish, all in Eden's pixel style.
- **Rain ambience and thunder sounds.**
- **Settings › Video › Atmosphere** (Off / Low / High) controls the cost. Older devices such as the iPad Air 2 stay on Off. Off renders exactly as before.

## Controls and movement
- **Srclike movement** (Settings › Experiments, off by default): Ground and air acceleration, strafe jumping, bunnyhopping.
- **Leaving noclip or fly keeps your momentum** instead of stopping you dead in mid-air.
- New *Noclip speed* setting under Gameplay.

## Settings menu
- **Sub-pages:** some settings are grouped into separate pages.
- **New World tile** gathers day/night, weather and save options in one place.
- **Video › Atmosphere details** with a Custom tier; new *UI scale*, *Input mode*, *Master volume* and *Mute in background* settings.


# Beta 0.3
Everything below is new since Beta 2 (0.2). Please send bug reports, suggestions and questions to the [Discord server](https://discord.gg/SgEQfurMmj).

## Heads-up
- **Mac and iPad: your settings start from defaults once.** Settings, key bindings, console history and your multiplayer name now live in one small preferences file on every platform (they used to be in the system defaults on Apple devices), so they reset the first time you run this version. Worlds are not affected.
- **iOS 15 or later is now required.** Devices on iOS 12–14 can no longer install this version.

## Graphics
- **Fixed a bug causing thin strips of other blocks to bleed into some texture edges on certain devices** (most visible on clouds). Texture coordinates now run at full precision on the GPU.
- **iOS render scale is back** (Settings › Video: 50 / 75 / 100 %). The graphics presets set it too, and older iPads such as the Air 2 default to 75 %.
- **Signs** are drawn with the planks texture and a wooden post instead of flat colour.
- Better-suited defaults on lower end devices.
- Improved frame limiter performance on very fast devices.
- Lower GPU power use: the world shaders no longer use `discard`, which let the GPU skip hidden surfaces. The frame limiter now sleeps instead of spinning, which lowers CPU use.

## Multiplayer
- **Fixed: player name tags** were mirrored through the screen centre and sat far from the players; they now appear above their heads.

## Controls and menus
- **Fixed: the collapsed console/chat bar's Back and Menu buttons did nothing on touch**, which blocked the console and multiplayer menu on phones and tablets.
- **The on-screen keyboard no longer covers the text field** you are typing in.
- Text no longer overflows buttons (multiplayer *Add server*, *Playing as*, World Properties); chat and console input no longer flicker in size while typing and use a tighter line gap.
- Scrollbar and slider highlights are thin fixed strips, like button highlights.
- World names accept only letters, digits, spaces and apostrophes, as in Eden 2.1.
- Weeds are in the block picker (with a proper icon), and bedrock can no longer be picked.
- Removed the experimental bunny-hop movement option.
- New-install defaults: autosave every 5 minutes, upgrade-to-64z on, backups off, classic shadows, crosshair chosen by input device.

## Under the hood
- Creature models are now loaded and animated by Emod's own code instead of the PowerVR SDK (whose licence was unclear). They look and move exactly as before.
- The game now draws everything through its own renderer; the old OpenGL ES 1.1 translation layer is gone. Nothing should look different.
- The Objective-C runtime emulation and Foundation shims were removed; the code builds as plain C++20 with one shared source list.

# Beta 2 (0.2)
Everything below is new since Beta 1. Please send bug reports, suggestions and questions to the [Discord server](https://discord.gg/SgEQfurMmj).

## Multiplayer (beta)
- **Join multiplayer servers** from a new server list: add servers by IP, with live server info, plus opt-in matchmakers that list public servers for you. Uses the vanilla Eden wire protocol. The screens are marked with a **Multiplayer BETA** badge.
- Block edits, signs and burning go out and come in live; other players appear in the world with **name tags** above their heads.
- **In-game chat** with a scrolling feed. Tapping the chat button opens a slim, semi-transparent input bar so the game stays visible behind it.
- Creatures are switched off during a session, and their models no longer stay frozen in the world.
- Sturdier networking: bad or slow servers, cancelled connects and dropped links are handled without hanging or corrupting the world.

## Controls
- **Fly modes:** `V` (or gamepad Y) cycles **Off → Fly → Noclip** with an on-screen toast. Noclip passes through blocks at high speed and makes you invulnerable. Settings › Gameplay › *Allow fly* turns the feature on (off by default).
- **Touch fly buttons:** dedicated fly up / fly down buttons on the HUD, replacing the old trick of borrowing the fire and pickaxe buttons, plus a button to cycle the fly mode.
- **New touch camera:** smoother, more precise look control with a small dead zone so taps and aiming no longer jitter the camera. Settings › Controls has new sliders for look speed, vertical speed, smoothing and an optional **glide** (the view coasts to a stop; off by default).
- Fixed: after a tap to break or place a block, the look finger stopped turning the camera while the move stick was held.

## Console
- The console's menu button reads **Menu** while collapsed, so it's clear the rail holds more (Chat, Console, Sessions).
- With the on-screen keyboard up, the hidden tab rail now leaves a hint instead of vanishing.

## Rendering
- **Third-person camera** culling and transparent sorting now use the real eye position, which fixes missing or mis-sorted faces at chunk boundaries.
- **Shadows** re-shade exactly the chunks an edit can affect, so shadows follow block edits correctly.
- Fixed: chunks containing only a door, flower or golden cube sometimes never showed them.
- Golden cubes rotate once per frame instead of once per cube (faster with many cubes).
- Fixed a rare crash from stopping sounds that were still playing when an effect unloaded.
- Fixed a fire list overflow and an overwritten firework slot.
- Fixed portal painting using swapped arguments.
- Fixed TNT explosions reading blocks outside the loaded area.

## Performance and memory (backend)
- **Lower memory:** the menu now holds far less (about 33 MiB was avoidable). Game textures are loaded only when a world starts, source PNG bytes are released after decoding, undrawn menu textures are skipped, and a measured memory leak in object buffers is fixed.
- **Non-power-of-two textures** upload directly instead of being padded, and **two-channel textures** (font and UI masks) upload at half the size.
- **Fewer GL calls and copies:** vertex data streams through ring buffers, with fewer driver stalls and less sign-renderer buffer churn.
- **Lighting:** one light lookup per block instead of three when computing light.
- **Console:** per-tab line caching, so large scrollback is cheap to draw.
- **Downloads:** each Get Worlds download uses its own temp file, so a cancelled download can no longer corrupt the next one, and leftover temp files are cleaned up.
- Chunk update lists are range-checked, and the save-cap bookkeeping keeps a running byte count instead of rescanning.
- The mesher and streaming code paths got further tuning, validated with new frame-time and pass-timing probes. Frame time on the iPad Air 2 stays at 60 fps.
- Dead code was removed (about 9,000 lines of unused PowerVR helper code) for a smaller binary and faster builds.

## Platforms and tooling
- A new command line option, `--docs=<dir>`, runs the game against a separate data folder without touching your saves.
- New automated regression tests (look, fly/noclip, touch, multiplayer replay) are run in CI, each with a mutation leg that checks the test still catches the bug.
- Settings now reset once on Mac and iPad (preferences move to one file shared by every platform).

# Beta 1 (0.1)
Eden: Community Edition (Emod) ports the 2010–2014 iOS release of *Eden: World Builder* (version 2.1.1, Objective-C++ over
OpenGL ES 1.1) and adds new community-designed features, backports newer vanilla features, and restores some lost functionality (like terrain generation and shadows). Long standing bugs are fixed, notably the corruption bugs arising from Eden's file format, which has been reworked entirely.

This was the first beta release of Emod.

## Cross-platform native port
- **Runs on macOS, Linux, Windows and iOS from one codebase**, instead of iOS only. The engine in
  `Classes/` is unchanged in spirit; a seam/shim layer (`port/`) replaces UIKit/EAGLView/CADisplayLink
  with SDL3 + GL and translates the engine's GL ES 1.1 calls to ES3/GL. Windows and Linux builds are
  verified in CI; macOS and iOS are played by a human.
- **Android is planned next** (not yet implemented).
- A large set of developer-facing `--*-selftest` / `--stage1` / `--shot` CLI modes (headless
  regression tests, deterministic screenshot capture, benchmarks) that have no equivalent in the
  shipped iOS app at all.

## In-game console & command-block scripting (new, Emod-only)
- **A typed, in-game console** (`` ` `` to open, or the pause menu's *Console* button) that runs while
  the world keeps playing. It shares one grammar and interpreter (`CmdScript`) with the game's
  command-block language, so anything a command block can do, a typed line can do too, anchored on
  the cell under the player's feet (or a fixed anchor via `anchor`).
- **A console verb/variable registry** (`Console::{h,mm}`): `help`, `find`, `cvars`, `toggle`,
  `reset`, `history`, `echo`, `where`, `anchor`, `jobs`, `stop`, `save`, plus diagnostics-only verbs.
  Every Settings row and every keybind automatically becomes a dotted console variable
  (`video.fov`, `keys.jump`, `world.name`, `gen.*`, …), live-editable from the console.
  Values can be inspected and changed instantly without leaving gameplay — stock has no way to
  change settings without going through the menu, and no way to query/alter world state live.
  Writable: world name and gold; most `world.*`/`gen.*` are read-only in-game via the console.
- **A console screen UI** (`ConsoleMenu`) with scrollback, tab completion, command history (persisted
  across sessions), a Variables tab listing every cvar, and a Help tab listing every verb — effectively
  an in-game reference and REPL that didn't exist before.
- **A touch-aware on-screen-keyboard layout** for the console on mobile/tablet, so the keyboard never
  covers the input field.
- **A `README.txt` written into the player's save folder** documenting the console/command-block
  commands, auto-generated from the same tables that drive the in-game Help tab (kept in sync by
  `--console-selftest`).
- A harness-only `--live-cmds` front end for scripted/automated testing of the above.

Command blocks and signs themselves originate from a later (2026) closed-source update to the
official app that 2.1.1 predates; Emod independently implements placing, editing, rendering and
running them (`SignTool`, `SignRenderer`, the `CmdScript` interpreter) and round-trips their on-disk
representation faithfully. 2.1.1 as shipped has neither feature.

## World management & sharing
- **World Browser** (`WorldBrowser`) — a new "Get Worlds" screen that replaces the stock
  NSURLConnection/UIKit browser with one that talks to the same official servers (current and legacy
  edengame.net endpoints) **plus** a third source: the community's own public world archive
  (`hagg3.github.io/edenarchive`). Supports Featured/Recent/Search on the official servers and
  full-catalogue search on the archive, with progressive download, multi-layer archive unpacking
  (gzip/zip/zip-in-zip), and conversion straight into the new world format without ever writing an
  intermediate file.
- **World sharing / Upload** (`WorldShare`) — upload a world (with format picker: Automatic, Legacy64z,
  NewDawn256z, NewFormat256z) and its preview screenshot to the official server, reproducing the stock
  upload contract from scratch on top of the new networking seam. Stock's own uploader is not compiled
  on the port.
- **Export** to `.eden`/`.eden.gz` for sharing/backward compatibility, with a pre-flight report of
  exactly what content (block types above 63, signs, command blocks) a given export format would lose.
- **World Properties screen** (`WorldPropertiesMenu`/`WorldProps.h`) — a new per-world details/editor
  screen (replacing the old bare "Rename"): shows the world's last in-game photo, type, height, seed,
  saved land size, sign/command-block counts, golden cubes held, created/last-saved timestamps, and
  provenance (what it was converted from); lets you rename a world and, for Classic worlds, tweak
  terrain-generation options for not-yet-generated terrain.
- **New World screen** (`NewWorldMenu`) — replaces stock's two sequential alert dialogs (type, then
  height) with a single screen asking name, world type (Flat / Normal / **Classic**, new), and height
  (64 vs. 256-block-tall "New Dawn") up front, with a live preview of generated terrain for Classic
  worlds (seed entry/"Random" reroll, hilliness, caves, trees, flowers, clouds, world size/scale).
- World list rebuilt on a new GL widget kit (scrollable, variable-height rows) with working native
  delete/rename (stock's delete confirmation was a no-op on the port before this work).

## World generation
- **A third, runtime-seeded "Classic" world generator**, in addition to stock's flat generator and
  the offline-only default-map generator: any seed other than 0 or 333333 now actually generates
  varied, seeded 1.x-style terrain at runtime (hills, caves, trees, biomes) via configurable options
  (hilliness, cave density, tree/flower/cloud density, world size/scale). In stock 2.1.1 this code
  path existed but was dead (always fell back to the flat recipe), so every non-default seed produced
  a flat slab.
- **"Unbounded" Classic worlds** can extend past the ~1440-block edge-of-world wall that otherwise
  clamps player movement.
- A dedicated offline default-map generator workflow (`TerrainGen2`) that bakes the shipped
  2880×2880 default world, now driven through the new save format's compressor.

## New world save format (`.emod`)
- **A new world container format, `.emod`**, replacing `.eden` as the only format the game creates,
  plays or saves. It's an append-only log of CRC-32-checked, zstd-compressed records with one commit
  record per save, so a save is atomic (a torn/interrupted save rolls back cleanly, with no separate
  journal file) — stock's `.eden` saves could be corrupted by an interrupted write. Columns are
  compressed per-chunk and store only their occupied vertical bands, keeping file size proportional to
  actually-built height rather than the full world height.
- `.eden` (and `.eden.gz`/`.zip`) are now **import/export-only**: dropped-in legacy worlds are
  converted to `.emod` on first play and never opened for update again.
- Vendors **zstd** compression into the engine (`Classes/zstd/`) to implement the above; stock links
  only zlib.
- A from-scratch encoder/decoder (`EdenWorldStore`/`EdenWorldSource`/`EdenWorldExport`) and a
  reference/verification tool (`port/tools/emod.py`) with no shared code, used to cross-check every
  byte.
- A one-click, in-game "convert to 64-block height" / "upgrade to 256" path with loss reporting,
  which stock never offered as a round-trippable, lossless-where-possible operation.

## Rendering & lighting
- **Baked top-face shadows** (`Classes/Shadow.{h,mm}`), a Video setting (Off / Classic / Smooth)
  restoring a 1.x-era shading-under-overhangs effect that stock 2.1.1 had disabled (its shadow code
  was dead/commented out, always returning full brightness).
- **Signs**: in-world placeable, paintable, directional sign boards with custom text, rendered as
  textured billboards — a feature stock 2.1.1 (as opposed to a later closed-source update) does not
  have at all.
- **A GL widget toolkit** (`GLWidgets`) — buttons, toggles, sliders, steppers, text fields, scrollable
  lists, tab rails, icon rails, two-line rows, modal dialogs (`GLDialog`) — giving every menu a
  consistent, density-aware (desktop vs. touch) look, replacing stock's one-off per-screen drawing
  code and its UIAlertView-based dialogs (not usable on non-Apple targets).
- A density-correct text rasterizer fix so UI text is crisp on high-DPI displays and no longer
  double-prints in wide status-bar boxes (both stock-era rendering bugs, fixed as part of the new kit).
- Fixed-radius streaming window is now a **runtime-configurable render distance** (Settings › Video,
  4–16 chunks desktop / 4–10-12 on iOS, plus Low/Default/High presets), instead of stock's single
  fixed radius.

## Input & controls
- **Rebindable keyboard controls** (`KeybindsMenu`) — every action key can be remapped from a Settings
  tab, with deliberate support for one key driving two actions (e.g. Ctrl bound to both fly-down and
  crouch). Stock has no rebinding UI at all (hard-coded `switch` on scancodes).
- Hide-HUD (F1), screenshot (F2), and a new double-size (2×2×2) build brush toggle (X, gated behind an
  Experiments setting) — none of which exist in stock.
- A native, cvar-backed Settings screen reorganized into Gameplay/Audio/Controls/Keys/Video/
  Experiments/Saves tabs on the new widget kit, versus stock's flat paginated list.
