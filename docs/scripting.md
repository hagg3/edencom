# Mod scripts (Lua) — *Emod only*

K.4 (2026-11-03). Code: `Classes/ModScript.{h,cpp}`
(the runtime), `CmdScript::runNow` (the back end every world edit goes through), the vendored Lua 5.4.9 in
`port/src/shim/vendor/lua/` (`port/tools/vendor-lua.sh`). Gate: `eden_native --lua-selftest --docs=<fresh>`
(`port/src/harness/mode_lua.cpp`; `--lua-mutate=hook|sandbox|loadearly|caperror|usefilter|signnocache|storenotdirty` must fail it).

## What a script is

A mod pack (`docs/content-packs.md`) may list Lua sources:

```jsonc
{"format": 1, "id": "towers", "version": "1.0",
 "scripts": ["scripts/lib.lua", "scripts/main.lua"]}     // "blocks" may be left out when "scripts" is given
```

- Up to 16 paths, each a relative path inside the pack ending in `.lua`, at most 256 KiB each and 1 MiB together;
  **source only** (a file starting with ESC, Lua's bytecode signature, is refused). A bad entry refuses the whole
  pack (its other scripts could not run without it). `ContentPack::compileMod` reads the files whole at world
  entry, so a script never opens a file afterwards; `port/tools/pack.py` checks the same rules with the same lines
  (fixtures `mod-scripts`, `mod-scripts-only`). The script bytes are part of the pack hash (after the tiles), so a
  server's `mods=` list covers them.
- Each loaded pack with scripts gets **its own Lua state**: globals, memory and errors never mix between packs.
  The files run in list order, sharing that state's globals (a `lib.lua` first is the way to share helpers; there
  is no `require`). Each file's top level runs to its end or its first `eden.wait`; then every `load` handler, once the last file's top level has finished (a top level that the
  instruction budget pauses, or that earlier packs starve of the frame's share, still gets its `load` once, after).
- **When.** The states are created on the first frame of play after `World::loadWorld` (`ModScript::worldEntered`
  right after `ContentPack::loadMods`; `ModScript::update` starts them), survive a warp, and are closed when the
  world closes (`Terrain::unloadTerrain(true)`) or the next one loads. A **multiplayer session runs none** (no
  pack loads in a session; K.3-S, the server side, is deferred). A state's globals are never saved: what must
  outlive the world goes in `eden.store` (below).

A larger worked example is the bundled `port/tools/fixtures/packs/wonders/` (`docs/content-packs.md`, and its own
`README.md`): thirteen files sharing helpers through a global table, one feature per file, every event, all of it
driven by placing, mining, painting or standing on blocks (no console needed): throttled `tick` loops (`eden.wait`
inside a `tick` handler makes it skip frames), timed builds laid out facing the player, a breadth-first flood over
`eden.get`, and state rebuilt after a reload by a background scan (`warp_pads.lua`).

## The sandbox

| Available | Changed or removed |
|---|---|
| `assert error getmetatable ipairs next pairs pcall rawequal rawget rawlen rawset select setmetatable tonumber tostring type xpcall _G _VERSION` | `print` writes to the console (`[pack] text`, at most 50 lines a frame) |
| `coroutine` `string` `table` `math` `utf8` | `load(chunk [, name [, mode [, env]]])`: a **string** chunk, **text only** (the mode is ignored) |
| `eden` (below) | `collectgarbage`: only `"count"`, `"collect"`, `"step"` |
| | `setmetatable` refuses a metatable with a `__gc` field: **finalizers are not available to mod scripts** (Lua runs them with the count hook off, so a looping one would hang the game; QA.3) |
| | `string.dump` removed; `string.find/match/gmatch/gsub` raise `pattern too complex` past 10 M match steps per call (per `gmatch` iteration), and refuse a subject over 64 KiB (a backstop for the plain `find`) |
| | `string.rep` with an empty string and separator returns `""` at once; `table.move/insert/remove` raise `too many elements to move` past 2^24 elements (a lying `__len`, `table.move({}, 1, 2^62, 2)`) |
| | `dofile`, `loadfile` removed; `io`, `os`, `package`, `debug`, `require` **do not exist** (never compiled: the vendored tree has no `liolib.c`, `loslib.c`, `loadlib.c`, `ldblib.c`, `linit.c`) |

No filesystem, network, clock or process access exists to escape to. `math.random` is seeded per state.

## Budgets

| Limit (`ModScript::Limits`) | Default | What happens past it |
|---|---|---|
| `memBytes` per state | 8 MiB | the allocation fails: a catchable `not enough memory` error (a shrink never fails) |
| `allMemBytes` (every state, the console's too) | 64 MiB; **24 MiB** on a low-memory device (the `mobile_low` device class: `eden_device_class()`, a mobile device that is Air 2-class or has 3 GB of RAM or less) | same, for whichever pack's allocation crosses the total: that pack gets `not enough memory` and lives on, the others keep what they hold (`scripts` ends with the total; `--lua-selftest` leg 16) |
| `packPerFrame` instructions | 100 000 | the running handler is **paused to the next frame** (the count hook yields it) |
| `allPerFrame` (every pack) | 250 000 | same, for whichever pack is running |
| `maxPreempts` | 600 frames | a handler paused that many frames in a row without an `eden.wait` is **stopped** with an error |
| `hardPerResume` | 5 000 000 | code that can't pause (a `table.sort` comparator, a `gsub` callback, a metamethod called from C, a coroutine the script made itself) gets an error instead; also past 2× in a frame |
| `maxErrors` | 100 | the pack's scripts are stopped (`scripts` says why) |
| world edits | `CmdScript::WORK_PER_FRAME` (20 000, shared with command blocks) | the edit call pauses the handler to the next frame and carries on where it stopped |

The hook runs every 1000 instructions (`HOOK_EVERY`) of Lua code; it never runs inside a C function's own loop,
so the vendored Lua bounds every loop whose length a script chooses (above; the patch is
`port/tools/vendor-lua-emod.patch`, applied by `vendor-lua.sh`). Gate: `--lua-selftest` leg 12, each hostile call
erroring in under 2 s (they ran 20 s to forever before). A pack over its frame budget starts no new handler that
frame (events and ticks wait in their coroutines), so one pack can't starve the game; it can only slow itself.

## The `eden` API (version 2)

**Beta.** The scripting API may still change before 1.0; nothing here is a promise that a script written today runs
on every later build. What the project does do: `eden.version` is an integer that a build raises when it adds API
(this one is 2), version N contains everything in N-1 with the same meaning, and the **Since** column says which
version added each call and event (everything that shipped first is 1). Test for a feature with a nil check
(`if eden.sign then`; a missing function is `nil`); use `eden.version` for messages. A pack that cannot run without a newer API says so with
`"requires": ["emod>=2"]` (`emod` is a virtual pack whose version is the API version): an older build skips it as
`needs "emod" (not installed)`, this one as `needs Emod scripting 3 or newer (this game has 2)`.

Coordinates are the engine's **(x, height, z)**, absolute (the console's `=N`). Every world edit is built as a
command-block statement and run by `CmdScript::runNow`: the same parser (`parseLine` with console options and no
console verbs), the same refusals (bedrock, door/portal halves, a cell inside the player, a non-resident cell,
`TYPE_CUSTOM`), the same caps (fill ≤ 4096 cells, `run` chains 8 deep), the same toasts. A malformed argument (an
unknown block, a `;` or space in a name) **raises** the parser's message (`Unknown block: glas`); a refused cell
does not raise, it shows in the return values.
The `unbreakable` trait stops the player's tools only: `set`, `fill` and `cmd` replace an unbreakable block like
any other (`setCell` refuses bedrock, door and portal halves), while `explode` refuses one (checked 2026-11-21).

| Call | Returns | Same as | Since |
|---|---|---|---|
| `eden.set(x, y, z, block [, colour])` | `ok, why` (false when refused) | `set` | 1 |
| `eden.fill(x1, y1, z1, x2, y2, z2, block [, colour])` | `cells, refused, why` | `fill` | 1 |
| `eden.cmd(line [, ax, ay, az])` | `cells, refused, why` | any block line; anchored at the player's feet cell (or the anchor given); `wait` is refused (use `eden.wait`) | 1 |
| `eden.paint(x, y, z, colour)`, `eden.explode(x, y, z)`, `eden.fire(x, y, z)`, `eden.tp(x, y, z)`, `eden.run(x, y, z)` | `ok` | `paint` `explode` `fire` `tp` `run`. `fire` lights only a flammable block (`Terrain::burnBlock`): an `eden:firework` set and fired launches when its 4 s fuse burns down, bursting in its paint colour, like one lit with the fire tool (checked 2026-11-19); `paint` on a lightbox changes its light's colour | 1 |
| `eden.spawn(creature [, x, y, z])` | a creature handle, or `false, why` | `spawn`. K.6-4a: the handle is below ("Creatures"); `why` is `"creatures are off"`, `"every creature slot is in use"` or `"that spot isn't loaded"` (before K.6-4a a spawn that did nothing returned `true`) | 1 (handle 2) |
| `eden.creatures(x, y, z, r)` | an array of handles, nearest first | K.6-4a: the live creatures within `r` (over 0, up to 64; else raises) of the point, at most 64; empty with no world open | 2 |
| `eden.say(text)`, `eden.time(t)`, `eden.timefreeze(on)`, `eden.heal()`, `eden.hurt(n)`, `eden.gold(n)`, `eden.sound(n)`, `eden.flash()` | `ok` | the verbs of those names | 1 |
| `eden.weather([kind [, secs]])` | `kind, intensity, locked, precip` | AT.1c: with `kind` (`clear` `overcast` `rain` `storm` `snow`; names from `Weather::kindByName`, shared with the console `weather` verb and the `weather.now` cvar, QA.25) starts that spell now (`secs` optional); `precip` is what falls at the player (`none` `rain` `snow` `dry`). Errors while Gameplay > Weather is off | 1 |
| `eden.weatherlock(on)` | — | AT.1c: a locked spell never ends | 1 |
| `eden.get(x, y, z)` | `name, colour`, or nothing if the cell is not resident | — | 1 |
| `eden.sign(x, y, z [, face])` | `text, colour, face`, or nothing if no sign hangs there (no `face`: the first sign on that block) | the picker's SIGN tool, read-only. `face` is the record's: 0 -x, 1 +x, 3 top, 4 -z, 5 +z (2, under the block, is never used); `colour` is the board's colorTable index 1..54 | 2 |
| `eden.setsign(x, y, z, face, text [, colour])` | `true`, or `false, why` when refused | the SIGN tool's own commit (`SignTool::set`): the cell must be resident and a non-air block (the anchor rule); `text` is at most 95 bytes of printable ASCII (32-126), refused whole, never trimmed; empty or all spaces deletes the sign (`true` even if there was none); `colour` 1..54 (omitted: an edit keeps the board's, a new sign is charcoal 45). Saved with the world; a script's own sign does not fire `sign`. A multiplayer session raises | 2 |
| `eden.toast(text [, seconds])` | `true`, or `false` when this pack toasted less than 1 s ago (game time) | K.7-1a: the HUD status line (`statusbar::setStatus`), so an engine status replaces it and it replaces an engine status. `text` is at most 120 bytes (longer raises; control characters become spaces); `seconds` is clamped to 1..6 (default 3). Raises with no world open | 2 |
| `eden.ask(title, body, buttons)` or `eden.ask{title=, body=, buttons=, danger=}` | the button index `1..n`, or `nil, why` | K.7-1b (beta): raises a modal dialog and **pauses the handler like `eden.wait` until the player answers** (no timer, no work budget; it holds one of the pack's 64 coroutine slots). The world stops under it (`eden.clock()` holds). `buttons` is 1..4 strings of at most 24 bytes, the safe choice **last** (Escape picks the last button); `danger` is the index drawn red. `title` ≤ 60 bytes, `body` ≤ 400 (a longer body is cut with `...` to fit the screen), control characters become spaces; a violation, or an unknown key, raises with what is wrong. `why`: `"muted"` (the player chose *Stop dialogs from this mod*; until the world closes), `"busy"` (this pack already has a dialog waiting its turn, or four wait in all), `"replaced"` (an engine dialog opened over it). Raises inside a callback that cannot pause and inside `leave` (`can't pause while the world closes`). Unprompted dialogs from `load`, `tick` or a timer are allowed, bounded by: one mod dialog at a time (the rest queue, one per pack), 0.5 s of game time between a pack's dialogs, a `Mod: <pack>` footer on every dialog, and from a pack's second dialog within 60 s that footer gains the *Stop dialogs from this mod* link. The answer is delivered at the start of the next frame (before the frame's edits are meshed) | 2 |
| `eden.input(title, body [, default])` or `eden.input{title=, body=, default=, max=64, lines=1, ok="OK", cancel="Cancel"}` | the text, or `nil, why` | K.7-1b (beta): `eden.ask` with a text field (`max` 1..256 bytes, `lines` 1..4, `ok`/`cancel` ≤ 24 bytes). OK returns the text, Cancel and Escape `nil, "dismissed"`; also `"muted"`, `"busy"`, `"replaced"` as above, and `"unavailable"` on a host with no text entry (nothing is shown) | 2 |
| `eden.setting(id)` | the player's value: a boolean (toggle), number (slider), or string (choice's option, text) | K.7-2b (beta): one of this pack's `pack.json` `"settings"` ([content-packs.md](content-packs.md) "Settings"), read from `Packs/.settings/<pack id>.json` on the first call and kept until the world closes (the main-menu Mods screen's page cannot open in a world; the pause menu's Mods page can, and updates the kept value). A stored value that fails the pack's schema reads as the default. An id the pack does not declare raises | 2 |
| `eden.menu{title=, tabs=\|rows=, on_action=, on_close=}` | a menu handle, or `nil, why` | K.7-3a (beta): a full-screen menu over the running world, below ("Menus") | 2 |
| `eden.blockid(name)` | the byte this world uses for a name, or nothing | `CmdScript::blockByName` | 1 |
| `eden.player()` | `{x, y (feet), z, yaw, pitch, health 0..1, dead, gold, cell_y, mode, colour, block}` | `cell_y` is the cell **under** the feet (the block stood on; `eden.cmd`'s anchor), so the air the player stands in is `cell_y + 1`. `yaw` is in degrees, forward is `(cos yaw, sin yaw)` in (x, z). K.6-7: `mode` is the tool in use (`camera` `pick_block` `build` `mine` `burn` `paint` `pick_color`), `colour` the paint colour index, `block` the block name in hand (absent for a tool, or a mod block not yet placed) | 1 (K.6-7: 2) |
| `eden.look()` | `x, y, z, face, block`, or nothing | K.6-7: the cell under the crosshair (the cell hit, not the empty cell a build would fill), the face of it that was hit (`face` as in `eden.sign`, plus 2 = its underside) and its block name. Nothing for sky, a creature, or with no world open | 2 |
| `eden.clock()` | seconds of game time since the scripts started | — | 1 |
| `eden.wait(seconds)` | — | pauses this handler (0..600 s, game time; 0 = next frame) | 1 |
| `eden.on(event, fn)` | — | at most 32 handlers an event | 1 |
| `eden.command(name, help, fn)` | — | a console command: `mod NAME ARGS...` calls `fn(args...)` with strings. `mod PACK:NAME` always runs that pack's; the short `NAME` goes to the first pack (load order) that registered it. A second pack registering a name another pack holds is a console warning, not an error: its file runs to the end. The same name twice in one pack still raises | 1 |
| `eden.store.get(key)` | the value stored under `key`, or `nil` (a **copy**: changing a returned table changes nothing) | per-world saved data (below) | 2 |
| `eden.store.set(key, value)` | `true`, or `false, why` when the value can't be stored or a cap is reached (never raises on a value or a size); `nil` deletes | marks the store dirty: it goes out with the world's next save | 2 |
| `eden.store.keys()` | an array of this pack's keys, in byte order | — | 2 |
| `eden.pack`, `eden.version` | the pack id, `2` | — | 1 |

**Editor support and a checker (K.8-8).** [`docs/eden-api.lua`](eden-api.lua) is a LuaLS stub file for `eden.*` (point `Lua.workspace.library` at it for completion and hover). It is generated: `eden_native --lua-api-dump > docs/eden-api.lua`, and CI fails when it drifts or when an API function has no doc line (`kApiDocs` in `Classes/ModScript.cpp`). `eden_native --check-pack=DIR` validates a pack folder the way the loader does, adds a Lua syntax check of every script (`file:line: message`), prints the lines and exits 1 on any error (warnings alone exit 0).

Blocks are names (`stone`, `eden:stone`, `mypack:jelly`) or numbers, like the verbs. A mod block with no byte in
this world yet gets one at its first placement (`BlockRegistry::acquire`, flagging the world, like the console).

## Creatures (handles)

K.6-4a/K.6-4b (beta). `eden.spawn` and `eden.creatures` give **handles**:

| Method | Returns |
|---|---|
| `h:valid()` | `true` while the handle's creature is loaded and alive |
| `h:pos()` | `x, y, z` (numbers, absolute): the creature's origin. A spawn at cell `(x, y, z)` reads `x+0.5, y+0.5, z+0.5` until it moves |
| `h:kind()` | the creature's name, as `eden.spawn` takes it (`moof` `batty` `green` `nergle` `stumpy` `charger` `stalker`) |
| `h:remove()` | `true`: the creature is gone at once, silently (no sound, no puff; it is not saved) |
| `h:vel()` | `vx, vy, vz` in blocks a second (its own walking included) |
| `h:setpos(x, y, z)` | `true`, or `false, why` (`that spot isn't loaded`, `that spot is solid`): moves the creature's origin exactly there, at rest; it then falls as from a spawn. Solid is anything a creature collides with, so air, flowers and water are open. The cell holding the point is what is checked; a body that overlaps a neighbouring block is pushed out the next frame |
| `h:setvel(vx, vy, vz)` | `true`: a **launch** (below) |

**A handle is valid while its creature is loaded and alive.** When the creature dies (a sword, fire, TNT,
`h:remove()`), is picked up, its cell leaves the loaded area, or the world closes, the handle is gone for good:
`h:valid()` is `false` and every other method returns `nil`. A creature still there when you come back is a new one
to scripts: find it with `eden.creatures(x, y, z, r)`. A handle never reads another creature that took its place.

The same creature is the same value while its handle lives, so `==` and table keys work (`pets[h] = true`).
`tostring(h)` is `creature moof #12` (the slot) or `creature (gone)`. Handles are not saved and `eden.store` refuses
them (`false, "can't store a creature handle"`).

**`h:setvel` is a launch, not a held state.** The velocity is set once, its length clamped to 16 blocks a second
(`CREATURE_SPEED_MAX`); from then on gravity, friction, water and collisions act on it, the way a sword hit knocks a
creature back. While the launch runs the creature's AI neither walks nor decides; it ends when the creature is down
(or in water) and slower than 0.5 blocks a second, or after 3 s, and the AI carries on from idle. Calling `setvel`
every frame holds a creature on a course; to add to its motion, `local x, y, z = h:vel(); h:setvel(x + dx, y, z)`.
A non-finite or huge number (`nan`, `math.huge`, over 10^9) raises in `setpos` and `setvel`: it would reach the
engine's integer casts and the save.

## Settings changes and the pause-menu Mods page (K.7-3b)

The pause menu has a *Mods* button when a loaded pack declares `pack.json` settings or registered `eden.on("menu", fn)`;
it opens a page with one tab per such pack. Two events go only to the pack concerned:

- `eden.on("setting", function(id, value, old) end)`: the player changed one of this pack's settings there (boolean, number
  or string as `eden.setting` returns it; a slider snapped to its step). Fires once per change, after the file is saved, so
  `eden.setting(id)` already returns the new value. A drag within one step fires nothing.
- `eden.on("menu", function() end)`: the player tapped the pack's *Open* row. The page has closed by then, so the handler can
  call `eden.menu{...}` at once.

Neither runs in a multiplayer session (no packs run there).

## Menus (`eden.menu`)

K.7-3a (beta). A pack's own screen: Back, its title, a `Mod: <pack>` footer, and rows the player scrolls, taps or tabs
through. **The world keeps running under it** (like the Console: creatures move, `tick` and timers fire, `eden.clock()`
advances); the sky and weather clocks stop as under the pause menu, and the menu takes every touch and key, so the player
cannot walk or build while it is open.

```lua
local m = eden.menu{
  id = "circuits", title = "Circuits",
  tabs = {
    {id = "wires", label = "Wires", icon = "cable", rows = {
      {type = "heading", label = "Power"},
      {type = "text", label = "Tap a row to place it where you stand."},
      {type = "row", id = "cable", label = "Place a cable", detail = "12 left", icon = "cable", hint = "One block of wire"},
      {type = "toggle", id = "glow", label = "Glow when powered", value = true},
      {type = "slider", id = "delay", label = "Delay", min = 0, max = 5, step = 0.5, value = 1.5},
      {type = "choice", id = "mode", label = "Mode", options = {"Pulse", "Steady", "Toggle"}, value = 2},
      {type = "button", id = "wipe", label = "Remove every wire", action = "Remove", danger = true},
    }},
    {id = "tools", label = "Tools", icon = "hammer", rows = {...}},
  },
  on_action = function(m, row, value) ... end,   -- row = the row's id; value below
  on_close = function(m) ... end,
}
```

- **Rows.** `heading`, `text` (a paragraph, `label` ≤ 400 bytes), `row` (a tappable line: `detail` on the right, `icon`,
  `hint` under it), `button` (`action` is its text, ≤ 16 bytes; `danger` draws it red), `toggle` (`value` true/false),
  `slider` (`min` < `max`, `step` > 0, at most 1000 steps; `value` defaults to `min` and is snapped to a step) and
  `choice` (2..8 `options` of ≤ 24 bytes; `value` is the 1-based index). There is no text-entry row: ask with
  `eden.input`. `row`, `button`, `toggle`, `slider` and `choice` need an `id` (`[a-z0-9_]{1,24}`, unique in the menu) and
  take `enabled = false` (drawn grey, not tappable) and `hint` (≤ 120 bytes). `label` is ≤ 40 bytes (a `text` row's 400).
- **Tabs.** `tabs` is 1..8 tabs, each `{id, label, icon, rows}` (`id` and `label` needed when there are two or more);
  or give `rows` instead of `tabs` for a one-page menu. With two or more tabs that all have an icon they are an icon rail
  down the left; otherwise a strip of labelled tabs across the top. At most 48 rows a tab and 128 in the menu.
- **Icons** are Lucide names from the game's atlas: `zap cable box package hammer pickaxe map compass flag star heart
  shield key gift clock sun moon cloud flame droplet leaf bug home door-open arrow-up arrow-down arrow-left arrow-right
  check x plus minus search trash-2 pencil eye bell music book coins`, plus the game's own (`settings`, `info`, `play`,
  `users`, `terminal`, `lock`, `trees`, `mountain`, ...: `Classes/UIIcons.h` `kUIIconNames`). A name, once in a build, keeps
  its glyph in every later build. An unknown name draws the `settings` gear and prints one console warning per name (a
  pack written for a newer build still opens). A pack's own PNG icons (`pack.json` `"icons"`) are named
  `"<pack id>:<icon>"`, e.g. `icon = "emod_example:spark"`; one the world does not have (pack off or removed) draws the
  gear and warns the same way.
- **Validation.** The whole table is checked when `eden.menu` is called; a cap broken, a missing key, an unknown row
  `type` or an unknown key raises with its path: `eden.menu: tabs[2].rows[7].label is longer than 40 bytes`, `rows[1].colour:
  unknown key (type 'row')`. Control characters in text become spaces.
- **`on_action(m, row, value)`** runs, as an ordinary handler (it may `eden.wait`, `eden.ask`, edit the world), when the
  player taps a `row` or `button` (`value` nil), flips a toggle (true/false), moves a slider (the number: a drag reports
  at most once a frame) or steps a choice (the 1-based index). It runs in the same frame as the tap, before that frame's
  edits are meshed, so a block it sets shows at once. The menu holds the new value (a later `m:set` or reopen keeps it).
- **`on_close(m)`** runs once each time the menu closes while the world stays open: Back, Escape, the menu key, or
  `m:close()`. Closing the world closes the menu without it.
- **The handle.** `m:set(row_id, {label=, detail=, hint=, value=, enabled=, action=, danger=, icon=})` changes a row (on
  screen at once; a value outside the row's range raises) and returns `true`; `m:tab(tab_id)` shows a tab (`false` if
  there is none); `m:close()` returns `true`, or `false` when it was not open; `m:open()` shows it again (`true`, or
  `false` while another mod menu is open or the pack is muted). The menu lives while the script keeps the handle (and
  while it is open); drop the last reference and it is collected.
- **`why`**: `"busy"` (a mod menu is already open: one at a time, across all packs) or `"muted"` (the player stopped
  this pack's dialogs). `eden.menu` raises with no world open, in a multiplayer session and inside `leave` (`can't pause
  while the world closes`). Opening a menu closes the Console; an `eden.ask` dialog draws over it.

## Saved data (`eden.store`)

Each pack has a key-value store **per world**, saved in the world's `.emod` as record 13 `MOD_DATA`
(`docs/emod-file-format.md`) in the same commit as its blocks, so a crash never leaves the two disagreeing. It
lives in the host (`ModScript.cpp`), not the Lua state, so it survives `scripts reload` and a warp; it is loaded
when the world opens (`FileManager::finishLoadOnMain`) and dropped when it closes. A set marks it dirty and the
bytes go out with the next save of any kind (autosave, background save, warp, exit), so keep state there as it
changes rather than at exit. A multiplayer session has none (no pack loads there).

- **Keys** are strings of 1–64 bytes (anything else raises). **Values**: booleans, numbers (an integer stays an
  integer, a float is kept bit for bit; NaN and infinities are refused), strings of up to 4,096 bytes of any
  bytes, and tables of those with string (1–64 bytes) or integer keys, nested at most 4 deep, at most 256 entries
  a table (a cycle is refused as too deep). Functions, coroutines and userdata are refused
  (`false, "can't store a function"`). Metatables are not consulted and not stored.
- **Caps** (each a `false, why`, nothing changed): a pack's whole store encodes to at most 64 KiB (`this pack's
  store is full (64 KiB)`); the world's record holds at most 64 packs and 1 MiB.
- **What the game never rewrites:** the entry of a pack that is not installed or not enabled, or that only read
  its data, is written back byte for byte, so removing a pack and adding it again loses nothing. Saved bytes that
  don't decode (a newer build's encoding) start the pack with an empty store and one console line, and are kept
  until the pack stores something. A world record that breaks the format's rules is ignored whole (one console
  line) and stays in the file until a pack stores something.
- **Encoding** (the runtime's; the format treats an entry as opaque bytes): `u8` version 1, then for each key in
  byte order `u8` length, the key, a value. A value is a tag: 0 false, 1 true, 2 `i64`, 3 `f64`, 4 `u32` length +
  bytes, 5 `u16` count + count × (key value, value) where a key value is tag 2 or 4, integer keys ascending then
  string keys in byte order. All little-endian. The same table always encodes to the same bytes.
- **Sharing:** the record travels inside the `.emod`; exporting to `.eden` drops it.

`--lua-selftest` leg 22: every value type survives save + reopen exactly, each refusal and cap, the entry of a pack
that is not loaded and an undecodable blob kept byte for byte over two saves, the blob rewritten once its pack
stores something, the 1 MiB and 64-pack record caps, and a session loading none.

**Sharing.** Share › Export › *World copy (.emod)* asks once whether to include a world's saved mod data (Include / Leave out / Cancel); `.eden` export and upload never carry it.

## Events

| Event | Arguments | When | Since |
|---|---|---|---|
| `load` | — | once, when the pack's last top level has finished (it can be frames after the first frame of play: a long top level is paused, a starved one starts late; a `load` handler registered at its end still fires). It can come before the player has landed and the land round them is loaded (`eden.get` under the feet answers nothing): a handler that builds round the player waits for that first (checked 2026-11-21) | 1 |
| `tick` | `dt` | every frame; a handler still running (waiting or paused) skips frames rather than piling up | 1 |
| `build` | `x, y, z, block` | the local player built a block (each cell of a double brush; jump-and-build) | 1 |
| `mine` | `x, y, z, block` (what was there) | the local player removed one | 1 |
| `paint` | `x, y, z, colour, block` | the local player painted one | 1 |
| `burn` | `x, y, z, block` | the local player set fire to a flammable block | 1 |
| `use` | `x, y, z, block, how` | the local player tapped a non-air block with the fire tool, flammable or not (`how` is `"fire"`), or tapped a block with the `usable` trait in build mode (`how` is `"tap"`, K.6-1b: nothing is built; [content-packs.md](content-packs.md) traits). `burn` is unchanged and, for a flammable block, fires after `use` in the same frame. `use` cannot cancel anything | 2 |
| `sign` | `x, y, z, face, text` | the local player placed or edited a sign with the SIGN tool (not a deletion: an emptied sign sends nothing). `x, y, z` is the anchor block, `face` as in `eden.sign`, `text` what is now on the board | 2 |
| `leave` | — | K.6-2: the world is really closing: Save & Quit (each try, if a save fails and the player tries again) or a clean app quit (the window closed), **before** the exit save, so what a handler sets or stores with `eden.store` is in it. Not on a warp (scripts outlive it), not when the app goes to the background (state that must survive belongs in `eden.store` as it changes: every save writes it), not on *Leave without saving*, never in a session. Each handler runs once to completion, in pack then registration order: `eden.wait` (and a `coroutine.yield` at the handler's top) raise `can't pause while the world closes`, the instruction budget does not pause it (past `hardPerResume` it errors, and the world still closes), and every world edit of every handler together gets one fresh `WORK_PER_FRAME`; past it an edit is refused with the why `the world is closing: its world-edit budget is spent` (nothing carries over: there is no next frame). For tidy-up | 2 |
| `weather` | `kind, previous` | AT.1c: the weather spell's kind changed (`clear` `overcast` `rain` `storm`; snow is rain-level with a snow flag, so it reads `rain`); fires from `Weather::update`, not player input; not in a session | 1 |

Events are queued at the player's intent sites (`MpWorld::intentBuild/Mine/Paint/Burn`; a fire tap queues `use` then `burn`, outside a session; at
most 256 a frame) and delivered once a frame in `World::update` **after `CmdScript::update` and before meshing**
(load-bearing order rule 7: edit before mesh). Edits made by scripts, command blocks or the console are not
events, so a handler can't trigger itself. Every handler call is its own coroutine (at most 64 live per pack); an
error ends that call only. At the cap a `tick` is skipped and a player event dropped: it is not an error, counts
toward neither `maxErrors` nor the toast, and is tallied in the pack's `skipped` count (`scripts`, `describe`); the
console says so at most once every 10 s. A `mod` command or `lua` line over the cap answers its caller with an error line.
`leave` is not queued: `World::requestExit()` calls `ModScript::leaving()` directly before `FileManager::saveWorld()`
(`docs/execution-flow.md`), and its handlers are exempt from the 64-coroutine cap (at most 32 per pack, as for any
event). `--lua-selftest` leg 23: a handler's block and store key are back after reopen, a second close fires it
again, `eden.wait` raises, edits past one budget are refused, an endless handler ends and the world still closes, a
warp fires nothing (`--lua-mutate=leaveaftersave`, saving first, must fail).

## Errors and the console

An error prints `[pack] <file>:<line>: <message>` to the console and stderr and toasts `Mod <pack>: script error
(see the console)` (at most every 10 s). Console verbs (Settings › Experiments › Developer console):

- `lua CODE` runs a line in the console's **own** state (same sandbox and API; an expression prints its values;
  one line, no `;` since `;` separates console statements). It can't see a pack's globals.
- `scripts` lists every state: handlers, waiting coroutines, memory (peak), instructions last frame, errors, `skipped` (cap), why
  it stopped. `scripts reload` reads every loaded pack's script files from disk again and restarts all states
  (`ContentPack::rereadScripts`, same caps and refusals as at world entry: a file that is unreadable, over 256 KiB, precompiled, or
  more than 1 MiB together is refused with a console line and that pack restarts on the sources it already had; a syntax error stops
  that pack with its line, the others run). `pack.json`, tiles and blocks are not re-read, so a changed `"scripts"` list needs the
  world reopened, and the pack hash (`modsHash`) stays the world-entry one until the next world entry. `--lua-selftest` leg 17.
- `mod` lists the commands scripts registered (`pack:name` for a name another pack holds first); `mod NAME ARGS...` runs one,
  `mod PACK:NAME ARGS...` runs that pack's (`--lua-selftest` leg 15).

## Testing a pack's scripts headless

`--live-cmds=<feed>` with `--packs=<pack>` and `/mod ...`, `/lua ...`, `/scripts` lines (see the headless-driving notes in `port/`). Two traps
found with the showcase pack (2026-11-03) and Wonder Blocks: the feed's `wait N` counts **frames**, and under `SDL_VIDEO_DRIVER=dummy` frames run
at thousands a second while `eden.wait` counts game seconds (150 000 frames was about 5 s), so a timed build needs a
huge `wait`; and `eden.get` answers nothing above the world's top (64 in a fresh test world), so a scan from above
must skip `nil` rather than stop. A creature launched with `h:setvel` moves sideways only with a frame cap (`/video.fps_cap 60` in the feed): at thousands of frames a second each frame's step is below a float's precision at x ≈ 65 000 and the creature only rises and falls (gravity, measured 2026-11-19: 20 blocks/s²). Events come from player input (plus `tick`, `load`, `leave`, AT.1c's `weather` and K.7-3b's `setting` and `menu`), so a test copy of a pack can wrap `eden.on`
in a first script to keep the handlers and call them from an `eden.command`.

## Implementation notes (for engine work)

- Lua is compiled as **C++** (`lua_onefile.cpp` includes the 27 `.c` files), so a Lua error is a C++ exception
  and a C function in `ModScript.cpp` that raises one still runs its destructors. Never include `lua.hpp`
  (`extern "C"` would not link).
- No Lua call that can allocate runs outside `lua_resume` or `protect()` (a `lua_pcall`'d trampoline): an
  unprotected error would reach Lua's panic, which aborts. What the host reads back after a resume is read without
  allocating or running a metamethod (`plainText`).
- After a yield pop **only the yielded values** (`lua_pop(T, nres)`): the rest of the stack is the paused C
  function's frame (an edit's job lives there). Clearing it crashed the continuation (found by `--lua-selftest`
  leg 6).
- The count hook yields only the coroutine the scheduler resumed (`Pack::running`) and only when
  `lua_isyieldable`; anywhere else it can only raise. Lua copies the hook and the extra space (the `Pack*`) into
  every coroutine from the thread that creates it.
- No script finalizer exists: `safeSetmetatable` refuses a `__gc` field, and Lua only marks an object for
  finalization inside `setmetatable` (adding `__gc` later does nothing). That matters because `lgc.c` `GCTM` runs a
  finalizer with `allowhook = 0`, at a collect and inside `lua_close`, where no budget could stop it. The only
  finalizer is the host's own `editGc` (C). `Pack::closing` still makes the hook raise at close.
- A creature handle (K.6-4a) is a `(slot, generation)` userdata (`emod.creature`, no `__gc`). The generation is
  `Model.cpp`'s `slotGen[]`, moved by every transition away from "this slot holds this live creature"
  (`docs/entities-and-creatures.md` "Script handles"); `CreatureHandleLive` also moves it when it finds a matching
  generation on a slot that is no longer live, so a transition no hook saw still ends the handle for good. One
  userdata per (slot, generation) is cached in a weak-valued registry table (`kCreatureCache`), which is what makes
  `==` and table keys work. `eden.spawn` learns its slot from `CreatureSpawns()` around the `runNow` call that runs it.
  Gate `--lua-selftest` leg 29; `--lua-mutate=nogen` (the generation ignored) must fail its reused-slot check.
- `h:setpos` / `h:setvel` (K.6-4b) go through `CreatureSetPos` / `CreatureSetVel` (`Model.h`); the launch is
  `Entity::knock` (`docs/entities-and-creatures.md` "Script handles"). Both refuse non-finite numbers in `vec3Arg`
  before anything reaches `guys[]`. Gate `--lua-selftest` leg 30; `--lua-mutate=nonfinite` (`setvel` takes a NaN)
  must fail its finite-velocity check (the leg puts the state back before a frame runs, so the mutant exits 1, not
  in a UBSan abort).
- Upstream Lua's C loops that a script sizes are patched (`MATCH_STEP` in `lstrlib.c`, `str_rep`, `TAB_MAXMOVE` in
  `ltablib.c`; every hunk is marked `Emod (QA.3)`). A re-vendor keeps them: `vendor-lua.sh` applies
  `port/tools/vendor-lua-emod.patch` and stops if it doesn't apply. Any new library function a script can reach
  that loops over a script-chosen count needs the same treatment.
