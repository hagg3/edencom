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
| `allMemBytes` (every state, the console's too) | 64 MiB; **24 MiB** on a low-memory device (the low-memory overlay, or 3 GB of RAM or less: `eden_script_low_memory()`) | same, for whichever pack's allocation crosses the total: that pack gets `not enough memory` and lives on, the others keep what they hold (`scripts` ends with the total; `--lua-selftest` leg 16) |
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

| Call | Returns | Same as | Since |
|---|---|---|---|
| `eden.set(x, y, z, block [, colour])` | `ok, why` (false when refused) | `set` | 1 |
| `eden.fill(x1, y1, z1, x2, y2, z2, block [, colour])` | `cells, refused, why` | `fill` | 1 |
| `eden.cmd(line [, ax, ay, az])` | `cells, refused, why` | any block line; anchored at the player's feet cell (or the anchor given); `wait` is refused (use `eden.wait`) | 1 |
| `eden.paint(x, y, z, colour)`, `eden.explode(x, y, z)`, `eden.fire(x, y, z)`, `eden.tp(x, y, z)`, `eden.run(x, y, z)` | `ok` | `paint` `explode` `fire` `tp` `run` | 1 |
| `eden.spawn(creature [, x, y, z])`, `eden.say(text)`, `eden.time(t)`, `eden.timefreeze(on)`, `eden.heal()`, `eden.hurt(n)`, `eden.gold(n)`, `eden.sound(n)`, `eden.flash()` | `ok` | the verbs of those names | 1 |
| `eden.weather([kind [, secs]])` | `kind, intensity, locked, precip` | AT.1c: with `kind` (`clear` `overcast` `rain` `storm` `snow`; names from `Weather::kindByName`, shared with the console `weather` verb and the `weather.now` cvar, QA.25) starts that spell now (`secs` optional); `precip` is what falls at the player (`none` `rain` `snow` `dry`). Errors while Gameplay > Weather is off | 1 |
| `eden.weatherlock(on)` | — | AT.1c: a locked spell never ends | 1 |
| `eden.get(x, y, z)` | `name, colour`, or nothing if the cell is not resident | — | 1 |
| `eden.sign(x, y, z [, face])` | `text, colour, face`, or nothing if no sign hangs there (no `face`: the first sign on that block) | the picker's SIGN tool, read-only. `face` is the record's: 0 -x, 1 +x, 3 top, 4 -z, 5 +z (2, under the block, is never used); `colour` is the board's colorTable index 1..54 | 2 |
| `eden.setsign(x, y, z, face, text [, colour])` | `true`, or `false, why` when refused | the SIGN tool's own commit (`SignTool::set`): the cell must be resident and a non-air block (the anchor rule); `text` is at most 95 bytes of printable ASCII (32-126), refused whole, never trimmed; empty or all spaces deletes the sign (`true` even if there was none); `colour` 1..54 (omitted: an edit keeps the board's, a new sign is charcoal 45). Saved with the world; a script's own sign does not fire `sign`. A multiplayer session raises | 2 |
| `eden.blockid(name)` | the byte this world uses for a name, or nothing | `CmdScript::blockByName` | 1 |
| `eden.player()` | `{x, y (feet), z, yaw, pitch, health 0..1, dead, gold, cell_y}` | — | 1 |
| `eden.clock()` | seconds of game time since the scripts started | — | 1 |
| `eden.wait(seconds)` | — | pauses this handler (0..600 s, game time; 0 = next frame) | 1 |
| `eden.on(event, fn)` | — | at most 32 handlers an event | 1 |
| `eden.command(name, help, fn)` | — | a console command: `mod NAME ARGS...` calls `fn(args...)` with strings. `mod PACK:NAME` always runs that pack's; the short `NAME` goes to the first pack (load order) that registered it. A second pack registering a name another pack holds is a console warning, not an error: its file runs to the end. The same name twice in one pack still raises | 1 |
| `eden.store.get(key)` | the value stored under `key`, or `nil` (a **copy**: changing a returned table changes nothing) | per-world saved data (below) | 2 |
| `eden.store.set(key, value)` | `true`, or `false, why` when the value can't be stored or a cap is reached (never raises on a value or a size); `nil` deletes | marks the store dirty: it goes out with the world's next save | 2 |
| `eden.store.keys()` | an array of this pack's keys, in byte order | — | 2 |
| `eden.pack`, `eden.version` | the pack id, `2` | — | 1 |

Blocks are names (`stone`, `eden:stone`, `mypack:jelly`) or numbers, like the verbs. A mod block with no byte in
this world yet gets one at its first placement (`BlockRegistry::acquire`, flagging the world, like the console).

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
| `load` | — | once, when the pack's last top level has finished (it can be frames after the first frame of play: a long top level is paused, a starved one starts late; a `load` handler registered at its end still fires) | 1 |
| `tick` | `dt` | every frame; a handler still running (waiting or paused) skips frames rather than piling up | 1 |
| `build` | `x, y, z, block` | the local player built a block (each cell of a double brush; jump-and-build) | 1 |
| `mine` | `x, y, z, block` (what was there) | the local player removed one | 1 |
| `paint` | `x, y, z, colour, block` | the local player painted one | 1 |
| `burn` | `x, y, z, block` | the local player set fire to a flammable block | 1 |
| `use` | `x, y, z, block, how` | the local player tapped a non-air block with the fire tool, flammable or not; `how` is `"fire"`. `burn` is unchanged and, for a flammable block, fires after `use` in the same frame. `use` cannot cancel anything | 2 |
| `sign` | `x, y, z, face, text` | the local player placed or edited a sign with the SIGN tool (not a deletion: an emptied sign sends nothing). `x, y, z` is the anchor block, `face` as in `eden.sign`, `text` what is now on the board | 2 |
| `weather` | `kind, previous` | AT.1c: the weather spell's kind changed (`clear` `overcast` `rain` `storm`; snow is rain-level with a snow flag, so it reads `rain`); fires from `Weather::update`, not player input; not in a session | 1 |

Events are queued at the player's intent sites (`MpWorld::intentBuild/Mine/Paint/Burn`; a fire tap queues `use` then `burn`, outside a session; at
most 256 a frame) and delivered once a frame in `World::update` **after `CmdScript::update` and before meshing**
(load-bearing order rule 7: edit before mesh). Edits made by scripts, command blocks or the console are not
events, so a handler can't trigger itself. Every handler call is its own coroutine (at most 64 live per pack); an
error ends that call only. At the cap a `tick` is skipped and a player event dropped: it is not an error, counts
toward neither `maxErrors` nor the toast, and is tallied in the pack's `skipped` count (`scripts`, `describe`); the
console says so at most once every 10 s. A `mod` command or `lua` line over the cap answers its caller with an error line.

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
must skip `nil` rather than stop. Events come from player input (plus `tick`, `load` and AT.1c's `weather`), so a test copy of a pack can wrap `eden.on`
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
- Upstream Lua's C loops that a script sizes are patched (`MATCH_STEP` in `lstrlib.c`, `str_rep`, `TAB_MAXMOVE` in
  `ltablib.c`; every hunk is marked `Emod (QA.3)`). A re-vendor keeps them: `vendor-lua.sh` applies
  `port/tools/vendor-lua-emod.patch` and stops if it doesn't apply. Any new library function a script can reach
  that loops over a script-chosen count needs the same treatment.
