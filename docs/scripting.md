# Mod scripts (Lua) — *Emod only*

K.4 (2026-11-03). Plan and decisions: `WORKING/mod-scripting-plan-2026-11-03.md`. Code: `Classes/ModScript.{h,cpp}`
(the runtime), `CmdScript::runNow` (the back end every world edit goes through), the vendored Lua 5.4.9 in
`port/src/shim/vendor/lua/` (`port/tools/vendor-lua.sh`). Gate: `eden_native --lua-selftest --docs=<fresh>`
(`port/src/harness/mode_lua.cpp`; `--lua-mutate=hook|sandbox` must fail it).

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
  is no `require`). Each file's top level runs to its end or its first `eden.wait`; then every `load` handler.
- **When.** The states are created on the first frame of play after `World::loadWorld` (`ModScript::worldEntered`
  right after `ContentPack::loadMods`; `ModScript::update` starts them), survive a warp, and are closed when the
  world closes (`Terrain::unloadTerrain(true)`) or the next one loads. A **multiplayer session runs none** (no
  pack loads in a session; K.3-S, the server side, is deferred). Runtime state is never saved.

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

## The `eden` API (version 1)

Coordinates are the engine's **(x, height, z)**, absolute (the console's `=N`). Every world edit is built as a
command-block statement and run by `CmdScript::runNow`: the same parser (`parseLine` with console options and no
console verbs), the same refusals (bedrock, door/portal halves, a cell inside the player, a non-resident cell,
`TYPE_CUSTOM`), the same caps (fill ≤ 4096 cells, `run` chains 8 deep), the same toasts. A malformed argument (an
unknown block, a `;` or space in a name) **raises** the parser's message (`Unknown block: glas`); a refused cell
does not raise, it shows in the return values.

| Call | Returns | Same as |
|---|---|---|
| `eden.set(x, y, z, block [, colour])` | `ok, why` (false when refused) | `set` |
| `eden.fill(x1, y1, z1, x2, y2, z2, block [, colour])` | `cells, refused, why` | `fill` |
| `eden.cmd(line [, ax, ay, az])` | `cells, refused, why` | any block line; anchored at the player's feet cell (or the anchor given); `wait` is refused (use `eden.wait`) |
| `eden.paint(x, y, z, colour)`, `eden.explode(x, y, z)`, `eden.fire(x, y, z)`, `eden.tp(x, y, z)`, `eden.run(x, y, z)` | `ok` | `paint` `explode` `fire` `tp` `run` |
| `eden.spawn(creature [, x, y, z])`, `eden.say(text)`, `eden.time(t)`, `eden.timefreeze(on)`, `eden.heal()`, `eden.hurt(n)`, `eden.gold(n)`, `eden.sound(n)`, `eden.flash()` | `ok` | the verbs of those names |
| `eden.weather([kind [, secs]])` | `kind, intensity, locked, precip` | AT.1c: with `kind` (`clear` `overcast` `rain` `storm` `snow`; names from `Weather::kindByName`, shared with the console `weather` verb and the `weather.now` cvar, QA.25) starts that spell now (`secs` optional); `precip` is what falls at the player (`none` `rain` `snow` `dry`). Errors while Gameplay > Weather is off |
| `eden.weatherlock(on)` | — | AT.1c: a locked spell never ends |
| `eden.get(x, y, z)` | `name, colour`, or nothing if the cell is not resident | — |
| `eden.blockid(name)` | the byte this world uses for a name, or nothing | `CmdScript::blockByName` |
| `eden.player()` | `{x, y (feet), z, yaw, pitch, health 0..1, dead, gold, cell_y}` | — |
| `eden.clock()` | seconds of game time since the scripts started | — |
| `eden.wait(seconds)` | — | pauses this handler (0..600 s, game time; 0 = next frame) |
| `eden.on(event, fn)` | — | at most 32 handlers an event |
| `eden.command(name, help, fn)` | — | a console command: `mod NAME ARGS...` calls `fn(args...)` with strings |
| `eden.pack`, `eden.version` | the pack id, `1` | — |

Blocks are names (`stone`, `eden:stone`, `mypack:jelly`) or numbers, like the verbs. A mod block with no byte in
this world yet gets one at its first placement (`BlockRegistry::acquire`, flagging the world, like the console).

## Events

| Event | Arguments | When |
|---|---|---|
| `load` | — | once, after the pack's files ran |
| `tick` | `dt` | every frame; a handler still running (waiting or paused) skips frames rather than piling up |
| `build` | `x, y, z, block` | the local player built a block (each cell of a double brush; jump-and-build) |
| `mine` | `x, y, z, block` (what was there) | the local player removed one |
| `paint` | `x, y, z, colour, block` | the local player painted one |
| `burn` | `x, y, z, block` | the local player set fire to a flammable block |
| `weather` | `kind, previous` | AT.1c: the weather spell's kind changed (`clear` `overcast` `rain` `storm`; snow is rain-level with a snow flag, so it reads `rain`); fires from `Weather::update`, not player input; not in a session |

Events are queued at the player's intent sites (`MpWorld::intentBuild/Mine/Paint/Burn`, outside a session; at
most 256 a frame) and delivered once a frame in `World::update` **after `CmdScript::update` and before meshing**
(load-bearing order rule 7: edit before mesh). Edits made by scripts, command blocks or the console are not
events, so a handler can't trigger itself. Every handler call is its own coroutine (at most 64 live per pack); an
error ends that call only.

## Errors and the console

An error prints `[pack] <file>:<line>: <message>` to the console and stderr and toasts `Mod <pack>: script error
(see the console)` (at most every 10 s). Console verbs (Settings › Experiments › Developer console):

- `lua CODE` runs a line in the console's **own** state (same sandbox and API; an expression prints its values;
  one line, no `;` since `;` separates console statements). It can't see a pack's globals.
- `scripts` lists every state: handlers, waiting coroutines, memory (peak), instructions last frame, errors, why
  it stopped. `scripts reload` restarts them from the sources read at world entry (edits on disk need the world
  reopened).
- `mod` lists the commands scripts registered; `mod NAME ARGS...` runs one.

## Testing a pack's scripts headless

`--live-cmds=<feed>` with `--packs=<pack>` and `/mod ...`, `/lua ...`, `/scripts` lines (port/CLAUDE.md). Two traps
found with the showcase pack (2026-11-03) and Wonder Blocks: the feed's `wait N` counts **frames**, and under `SDL_VIDEO_DRIVER=dummy` frames run
at thousands a second while `eden.wait` counts game seconds (150 000 frames was about 5 s), so a timed build needs a
huge `wait`; and `eden.get` answers nothing above the world's top (64 in a fresh test world), so a scan from above
must skip `nil` rather than stop. Events come from player input (plus `tick`, `load` and AT.1c's `weather`), so a test copy of a pack can wrap `eden.on`
in a first script to keep the handlers and call them from an `eden.command` (`TESTERS/wonders/testpack/test/`, driven by `TESTERS/wonders/run.sh`).

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
- Upstream Lua's C loops that a script sizes are patched (`MATCH_STEP` in `lstrlib.c`, `str_rep`, `TAB_MAXMOVE` in
  `ltablib.c`; every hunk is marked `Emod (QA.3)`). A re-vendor keeps them: `vendor-lua.sh` applies
  `port/tools/vendor-lua-emod.patch` and stops if it doesn't apply. Any new library function a script can reach
  that loops over a script-chosen count needs the same treatment.
