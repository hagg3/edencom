# Console and command-block reference

*Emod only.* What you can type, for people. How it is built (parser, registry, jobs, the screen's code) is
[console.md](console.md); the player-facing short version is the `README.txt` Emod writes into its worlds folder.

**The tables below between `<!-- BEGIN generated:… -->` markers are generated from the binary — never edit them.**
Regenerate with `port/tools/console-reference.py native/build/eden_native` (a `-DEDEN_DIAGNOSTICS=ON` build, so the
diagnostics rows are listed); `--console-selftest` fails if a verb, variable or group is missing from this file.

## Opening it, typing, leaving

- **Open:** the pause menu's *Console* button, or the `console` key (default `` ` ``; desktop). The world keeps running
  behind it, and every touch and game key belongs to the console until you close it.
- **Type** a line and press Return (or tap *Run*). **Tab** completes the word you are on (one match completes; several
  are listed and the line takes their common prefix). **Up/Down** step through the last 50 lines you ran (kept between
  sessions). On a touch screen the *Tab / Up / Down* buttons do the same. Tapping an old line in the scrollback puts it
  back in the input.
- **Tabs:** *Console*, *Variables* (every variable and its value; tap one to start typing it), *Help* (every verb; tap
  to start typing it). *Collapse* shrinks the console to a strip at the top so you can watch the world.
- **Close:** *Back*, the `console` key (only while the input is not focused), or Escape twice (the first leaves the
  input). Closing returns to play, not to the pause menu.
- The scrollback keeps 200 lines. A line is at most **511 bytes**.

## Lines, statements, jobs

A line is **statements separated by `;`**: `fill 0 0 0 3 0 3 stone; spawn moof 0 1 0`. They run in order, at once, up to a
`wait`.

- **A line with any mistake runs nothing.** Every mistake is printed as `N: message` (N = the statement number), then
  `line not run`. (A command *block* instead runs its good statements and toasts the bad ones, so a saved world's scripts
  never change meaning.)
- **`wait S`** (0 to 600 seconds, at least one frame) pauses *this line* only; the game and other lines keep running.
  The console prints `[job N] waiting S s`, and `[job N] done` at the end. At most **4** lines wait at once.
  `jobs` lists them, `stop N` ends one, `stop` ends every console job, `stop all` also stops every running command block.
  Closing the world stops everything.
- **`if X Y Z TYPE`** / **`unless X Y Z TYPE`** make the *next* statement conditional; `if A; if B; X` means A and B.
- **`run X Y Z`** pushes the command block at that spot; scripts chain up to **8** deep, with at most **32** command
  blocks running at once.
- Work is capped per frame (20,000 statements and filled cells); past that a line pauses until the next frame.

## Coordinates

A position is three numbers, **X, height, Z** (height is up). They are measured from the **anchor**:

| Written | Means |
|---|---|
| `0 0 0` | the anchor itself — by default **the block under your feet** |
| `3 1 -2` | 3 along X, 1 up, 2 back along Z from the anchor (within ±100,000) |
| `~`, `~5` | `~` alone is 0; `~5` is 5 (a leading `~` changes nothing) |
| `=64` | **console only:** the world's own coordinate 64 on that axis, not a distance from the anchor (within ±10,000,000). Mix freely: `tp =100 30 =100` |

`where` prints your cell, the anchor, and the cell you are looking at, each with its offset from the anchor.
`anchor look` moves the anchor to the cell under the crosshair (so you can test a command block's lines from the
console); `anchor X Y Z` fixes it; `anchor me` (or closing the world) puts it back under your feet. In a **command
block** the anchor is the block itself.

## Command blocks versus the console

| | Command block | Console |
|---|---|---|
| Anchor | the block | the cell under your feet (or `anchor`) |
| `=N` coordinates | not a coordinate | the world's coordinate |
| `@touch` / `@step` / `@near N` / `@timer N` header | allowed | refused |
| A mistake in the line | toasts it, runs the rest | runs nothing |
| Console verbs (`help`, `anchor`, variables…) | *Unknown command* | work, in the same line |
| Output | toasts | the scrollback |
| Waiting | an instance on the block (32 at once) | a job (4 at once) |

Everything a block can do, the console can do; a line you typed standing on a block does exactly what pressing that
block would (that is a tested guarantee).

## Block verbs

They work in a command block and in the console. Arguments: a **coordinate** (above), a **block** (a name such as
`stone`, `glass`, `air`, or a number 0–127), a **colour** (the stored paint byte, 0 = unpainted … 54), a **creature**
(`moof`, … `stalker`, or 0–6), a **number**.

<!-- BEGIN generated:block-verbs -->
| Verb | Usage | What it does |
|---|---|---|
| `say` | `TEXT` | Shows TEXT on screen as a status message. |
| `tp` | `X Y Z` | Teleports the player, feet landing at that cell. |
| `set` | `X Y Z TYPE [COLOR]` | Places or replaces one block; air removes it. Optional paint colour. |
| `fill` | `X Y Z X2 Y2 Z2 TYPE [COLOR]` | Fills the box between the two corners, up to 4096 blocks. |
| `paint` | `X Y Z COLOR` | Repaints an existing block (colour 0-54). |
| `explode` | `X Y Z` | Drops a lit TNT block there: real fuse, real blast. |
| `fire` | `X Y Z` | Ignites the block there, if it burns. |
| `spawn` | `CREATURE [X Y Z]` | Spawns a creature, on the anchor unless you give a spot. |
| `day` | - | Gives the region its day sky back (time 0.25). |
| `noon` | - | The day sky (time 0.5). |
| `night` | - | Paints the region's sky night (time 0.75). |
| `midnight` | - | The night sky (time 0). |
| `time` | `T` | Sets the time, T from 0 to 1: 0.25..0.75 is day, the rest night. |
| `heal` | - | Full health. |
| `hurt` | `N` | Deals N% damage. Only bites with Health on. |
| `gold` | `N` | Grants N golden cubes (the purse holds 999). |
| `sound` | `N` | Plays sound effect number N. |
| `flash` | - | White screen flash. |
| `wait` | `S` | Pauses this script S seconds (up to 600). Everything else keeps running. |
| `run` | `X Y Z` | Pushes the command block at that spot. Chains up to 8 deep. |
| `if` | `X Y Z TYPE` | The next statement runs only if that cell holds that block. |
| `unless` | `X Y Z TYPE` | The next statement runs only if it doesn't. |
<!-- END generated:block-verbs -->

Limits and refusals worth knowing:

- `set` / `fill` skip bedrock, one half of a door or portal, a cell inside the player, and cells outside the loaded
  area; a solid block will not overwrite a command block (`air` removes it, like mining). They never place doors,
  portals or custom blocks. The console prints `N cells set, M refused` whenever the line yields or ends.
- `fill` covers at most **4096** cells. `paint` only repaints a block that exists.
- `hurt` does nothing with *Health* off; `gold` fills a purse of at most **999**; `sound N` is a sound-effect number.
- `day`/`noon` give the region its day sky (time 0.25 / 0.5), `night`/`midnight` paint it night (0.75 / 0); `time T`
  takes any T from 0 to 1 (0.25 to 0.75 is day). Eden has no clock: this is a repaint of the region's sky.
- `spawn CREATURE` with no position spawns on the anchor.
- Example: `fill 0 0 0 4 0 4 stone; wait 2; fill 0 0 0 4 0 4 air` (a stone pad that vanishes after two seconds).

## Console verbs

Only in the console (a command block does not know them). Block verbs always win a name clash, so a saved world's
meaning can never change.

<!-- BEGIN generated:console-verbs -->
| Verb | Usage | What it does | Needs |
|---|---|---|---|
| `help` | `[VERB or VARIABLE]` | Lists the verbs, or explains one. | - |
| `find` | `TEXT` | Every verb and variable whose name or description has TEXT. | - |
| `cvars` | `[GROUP]` | Lists the variables (every one of a GROUP). | - |
| `toggle` | `VARIABLE` | Flips an on/off variable; steps a choice to the next option. | - |
| `reset` | `VARIABLE \| GROUP \| all` | Back to the default. `reset all yes` resets every setting and key. | - |
| `clear` | - | Empties the scrollback. | - |
| `history` | - | The lines you ran last. | - |
| `echo` | `[TEXT]` | Prints TEXT here only (say also toasts). | - |
| `where` | - | Your cell, the anchor and the cell you look at, with their offsets. | a world |
| `anchor` | `[me \| look \| X Y Z]` | What 0 0 0 means: the cell under you (me), the cell you look at, or a fixed cell. | a world |
| `jobs` | - | The lines still waiting on a `wait`. | - |
| `stop` | `[JOB \| all]` | Stops one job, every job, or all (blocks too). | - |
| `save` | - | Saves the world (the pause menu's Save). | a world |
| `getblock` | `X Y Z` | The raw type:colour of a cell. | a world, a diagnostics build |
| `cmdtrace` | `[0\|1]` | Prints every statement a script runs. | a diagnostics build |
| `stats` | - | The command interpreter's counters. | a diagnostics build |
<!-- END generated:console-verbs -->

- `help` alone lists every verb; `help NAME` gives a verb's usage or a variable's range and default. `find TEXT` searches
  names and descriptions of both.
- `reset all` only explains; `reset all yes` resets **every** setting and key (a typed line has no dialog).
- `toggle` flips an on/off variable and steps a choice variable to its next option; it refuses a number.
- `where` and `anchor` need a world open.

## Variables

A variable is `group.name`. **`NAME`** prints it, **`NAME VALUE`** sets it. Names are not case-sensitive.

- **on/off** variables take `on off 1 0 yes no true false`.
- **Number** variables take a number; out of range is clamped and says so.
- **Choice** variables take the option's label in any case, spaces optional (`saves.autosave 2min`), or its position
  (the label list below is in order; position 0 is the first).
- **Text** variables (`world.name`, the `keys.` group) take the rest of the line. A key takes a key name as shown in
  Settings (`Space`, `F9`, `` ` ``) or `none`; `keys.jump F` rebinds at once and the old key stops working.
- **Applies** says when a change takes effect: *at once*, *when a world opens*, *at the next conversion*, or
  *read-only in game* (the seed, size and generator values of an open world; change terrain in World Properties from
  the world list). `world.name` and `world.gold` are live.
- Every Settings row and every rebindable key is a variable automatically; the tables are the full list.

<!-- BEGIN generated:variables -->
#### `gameplay.`

| Variable | Values | Default | Applies | What |
|---|---|---|---|---|
| `gameplay.health` | on / off | on | at once | Falls, fire and creatures hurt. |
| `gameplay.autojump` | on / off | on | at once | Auto-jump |
| `gameplay.creatures` | on / off | on | at once | Off is faster. |

#### `audio.`

| Variable | Values | Default | Applies | What |
|---|---|---|---|---|
| `audio.music` | on / off | on | at once | Music |
| `audio.sound` | on / off | on | at once | Sound effects |
| `audio.music_volume` | 0 to 1 | 1 | at once | Music volume |
| `audio.ambience_volume` | 0 to 1 | 1 | at once | Wind and water. |
| `audio.effects_volume` | 0 to 1 | 1 | at once | Effects volume |
| `audio.night_ambience` | on / off | off | at once | Looping night sound when the sky is dark. |
| `audio.touch_controls_sound` | on / off | off | at once | Clicks on joystick and button taps. |

#### `controls.`

| Variable | Values | Default | Applies | What |
|---|---|---|---|---|
| `controls.input_mode` | Auto, Touch, Keyboard+Mouse | Auto | at once | Force one if Auto guesses wrong. (diagnostics builds) |
| `controls.hold_to_act` | on / off | on | at once | Repeat while the button is held. |
| `controls.mouse_sensitivity` | 0.25 to 3 | 1 | at once | Mouse sensitivity |
| `controls.mouse_sensitivity_y` | 0.25 to 3 | 1 | at once | Mouse sensitivity (Y) |
| `controls.invert_look` | on / off | off | at once | Invert look |
| `controls.gamepad` | on / off | on | at once | Use a connected controller. |
| `controls.gamepad_look_sensitivity` | 0.25 to 3 | 1 | at once | Gamepad look speed |
| `controls.gamepad_deadzone` | 0.05 to 0.5 | 0.15 | at once | Raise it if the camera drifts. |

#### `video.`

| Variable | Values | Default | Applies | What |
|---|---|---|---|---|
| `video.graphics_preset` | Custom, Low, Default, High | Default | at once | Low is faster; High sees further. |
| `video.fov` | 60 to 110 | 80 | at once | 80 is the original. |
| `video.view_distance` | Auto, 4, 5, 6, 7, 8, 9 | Auto | at once | In chunks. Lower is faster. |
| `video.render_radius` | 4, 5, 6, 7, 8, 9, 10, 12, 16 | 9 | when a world opens | Less is faster and lighter; more uses more memory. Applies when a world opens. |
| `video.shadows` | Off, Classic, Smooth | Off | at once | Shade ground under trees and roofs. |
| `video.display_mode` | Fixed, Fit window, Fullscreen | Fixed | at once | Display (diagnostics builds) |
| `video.ui_scale` | Auto, 100%, 125%, 150%, 200% | Auto | at once | UI scale (diagnostics builds) |
| `video.display_layout` | Auto, Classic 16:9, Adaptive | Auto | at once | Adaptive fits the window; Classic letterboxes. (diagnostics builds) |
| `video.render_scale` | 50%, 75%, 100%, 125% | 100% | at once | Lower is faster and softer. |
| `video.dpr_cap` | 1x, 1.5x, 2x | 2x | at once | Lower it if the frame rate suffers. (diagnostics builds) |
| `video.fps_cap` | Uncapped, 30, 45, 60 | Uncapped | at once | Lower saves battery and heat. |

#### `interface.`

| Variable | Values | Default | Applies | What |
|---|---|---|---|---|
| `interface.crosshair` | on / off | on | at once | Crosshair |
| `interface.block_preview` | on / off | off | at once | Shows where the next block goes. |
| `interface.legacy_menu` | on / off | off | at once | The original 2010 menus. (diagnostics builds) |

#### `experiments.`

| Variable | Values | Default | Applies | What |
|---|---|---|---|---|
| `experiments.fly` | on / off | off | at once | Space and Ctrl to rise and fall. |
| `experiments.fps_normalize` | on / off | on | at once | Same walk speed at any refresh rate. |
| `experiments.advanced_movement` | on / off | off | at once | Zero-delay bunny-hop and wheel-jump. |
| `experiments.crouch` | on / off | off | at once | Adds a crouch key for 1-block gaps. |
| `experiments.fire_melts_ice` | on / off | off | at once | Burning ice turns to water. |
| `experiments.fire_chars_vines` | on / off | off | at once | Burning vines turn to dark stone. |
| `experiments.creature_walk_voices` | on / off | off | at once | Creatures call out as they start walking. |
| `experiments.double_brush` | on / off | off | at once | A key builds 2x2x2 blocks at a time. |
| `experiments.camera_view` | First person, Third person, Third person (front) | First person | at once | Third person shows your body. F5 cycles. |

#### `saves.`

| Variable | Values | Default | Applies | What |
|---|---|---|---|---|
| `saves.save_backup` | on / off | on | at once | Keep the previous save as a .bak. |
| `saves.autosave` | Off, 1 min, 2 min, 5 min | 1 min | at once | Off still saves on exit and warp. |
| `saves.upgrade_256z` | on / off | on | at the next conversion | Room to build up to y 255. Uses more memory. |

#### `keys.`

| Variable | Values | Default | Applies | What |
|---|---|---|---|---|
| `keys.moveForward` | a key name, or none | - | at once | Move forward |
| `keys.moveBack` | a key name, or none | - | at once | Move back |
| `keys.moveLeft` | a key name, or none | - | at once | Strafe left |
| `keys.moveRight` | a key name, or none | - | at once | Strafe right |
| `keys.sprint` | a key name, or none | - | at once | Sprint |
| `keys.walk` | a key name, or none | - | at once | Walk (slow) |
| `keys.jump` | a key name, or none | - | at once | Jump / fly up |
| `keys.flyDown` | a key name, or none | - | at once | Fly down |
| `keys.crouch` | a key name, or none | - | at once | Crouch |
| `keys.fireTool` | a key name, or none | - | at once | Fire tool |
| `keys.flyToggle` | a key name, or none | - | at once | Toggle fly mode |
| `keys.blockPicker` | a key name, or none | - | at once | Block picker |
| `keys.colorPicker` | a key name, or none | - | at once | Colour picker |
| `keys.buildSize` | a key name, or none | - | at once | Build brush size |
| `keys.menu` | a key name, or none | - | at once | Menu / pause |
| `keys.console` | a key name, or none | - | at once | Console |
| `keys.blockPreview` | a key name, or none | - | at once | Toggle block preview |
| `keys.hideHud` | a key name, or none | - | at once | Hide HUD |
| `keys.screenshot` | a key name, or none | - | at once | Screenshot |
| `keys.cameraView` | a key name, or none | - | at once | Camera view |
| `keys.hotbar1` | a key name, or none | - | at once | Hotbar 1 |
| `keys.hotbar2` | a key name, or none | - | at once | Hotbar 2 |
| `keys.hotbar3` | a key name, or none | - | at once | Hotbar 3 |
| `keys.hotbar4` | a key name, or none | - | at once | Hotbar 4 |
| `keys.hotbar5` | a key name, or none | - | at once | Hotbar 5 |
| `keys.hotbar6` | a key name, or none | - | at once | Hotbar 6 |
| `keys.hotbar7` | a key name, or none | - | at once | Hotbar 7 |
| `keys.hotbar8` | a key name, or none | - | at once | Hotbar 8 |
| `keys.hotbar9` | a key name, or none | - | at once | Hotbar 9 |

#### `world.`

| Variable | Values | Default | Applies | What |
|---|---|---|---|---|
| `world.name` | text | - | at once | World name |
| `world.file` | text | - | read-only in game | File name |
| `world.seed` | text | - | read-only in game | Seed |
| `world.height` | text | - | read-only in game | Height (64 or 256) |
| `world.type` | text | - | read-only in game | Generator |
| `world.signs` | text | - | read-only in game | Signs |
| `world.cmdblocks` | text | - | read-only in game | Command blocks |
| `world.gold` | 0 to 999 | 0 | at once | The purse (0..999). |

#### `gen.`

| Variable | Values | Default | Applies | What |
|---|---|---|---|---|
| `gen.hills` | text | - | read-only in game | Hills |
| `gen.caves` | text | - | read-only in game | Caves |
| `gen.trees` | text | - | read-only in game | Trees |
| `gen.flowers` | text | - | read-only in game | Flowers |
| `gen.clouds` | text | - | read-only in game | Clouds |
| `gen.size` | text | - | read-only in game | World size |
| `gen.scale` | text | - | read-only in game | Scale |

<!-- END generated:variables -->

## Adding to it

- **A variable:** add a row to `kSettings[]` (`port/src/seam/Settings_web.mm`) or a keybind — `Console::init()` registers
  it with no console code. A feature with its own state calls `Console::addCvar(Cvar{…})` from its init path.
- **A console verb:** `Console::addCommand(name, usage, help, flags, argKinds, fn)` from the feature's init path (not a
  static initialiser). Flags: `CF_WORLD` needs a world, `CF_DIAG` exists only in diagnostics builds, `CF_EDITS` marks
  world changes, `CF_CHEAT` is reserved. No `.` in a verb's name; it may not repeat a block verb.
- **A block verb** is a row in `kVerbs[]` plus a case in `CmdScript::execute()` — it changes what saved worlds can
  contain, so read [console.md](console.md) and `Classes/CmdScript.h` first.
- Then run `port/tools/console-reference.py` and commit the result; `--console-selftest` fails until the file names the
  new thing.
