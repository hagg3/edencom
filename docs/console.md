# The console language (command blocks + typed lines)

*Not stock: Emod only.* Stage C (`WORKING/dev-console-plan.md`). **As of C.2 (2026-10-16)** the shared
parser and interpreter (C.1, `Classes/CmdScript.{h,mm}`) and the registry with its cvars and console
verbs (C.2, `Classes/Console.{h,mm}`) are in place, and `--console-selftest` gates both. The screen is
C.3 and its on-screen-keyboard layout C.4 (*The screen* below); `Console::run(line)` is the front door
the screen and the selftest share. Command blocks themselves (D.4c) are described in `CmdScript.h`'s header comment,
[ui.md](ui.md) (pushing one), [eden-file-format.md](eden-file-format.md) (the `CMB1` record) and
[execution-flow.md](execution-flow.md) step 5b.

## One grammar, two front ends

A console line **is** a command-block script: `;`-separated statements, the same verbs, the same
`execute()`, the same refusals and caps. A line that both a block and the console accept means the same
thing relative to its **anchor**. `CmdScript::parse(src, out)` is the block path and behaves exactly as
before C.1. `parseLine(src, out, opts)` is the shared path:

| `ParseOptions` field | Block (`blockOptions()`) | Console (`consoleOptions(ext)`) |
|---|---|---|
| `triggers` | an `@touch/@step/@near/@timer` header is allowed | any `@` statement is *Triggers only work in a command block* |
| `absolute` | `=N` is *Not a coordinate: =N* | `=N` is the engine coordinate N (`Stmt::abs[i]`); the bound is ±10,000,000 (relative coordinates keep ±100,000) |
| `ext` | none | the registry's verbs parse to `OP_EXT` (`Stmt::ext` = index, `Stmt::args` = the words). They are tried **after** every built-in verb, so a registry verb can never change a block's meaning; a block given one says *Unknown command* |

`Program::ext` records which registry parsed the program; `OP_EXT` calls `ext->run(index, ExtCall)` with
the anchor, the words, the sink, the source and the job id. Registry verbs are plain `if`/`unless`-gated
statements like any other: `if 0 0 0 grass; video.fov 100`.

## The verb table (`kVerbs`)

`CmdScript::verbs()` / `findVerb()` hold one row per built-in verb in reference order: name, usage
(`"X Y Z TYPE [COLOR]"`), the wording of its "needs" error when it isn't the usage (`spawn` → *a
creature*, `time` → *T from 0 to 1*, `hurt/gold/sound/wait` → *a number*), a one-line help, and the
argument kinds (`ARG_COORD/BLOCK/COLOR/CREATURE/NUMBER/TEXT`). The parser builds every *X needs …*
message from the row, so help, errors and (C.2) completion can't drift; `--console-selftest` checks
that every row parses and that each bare verb's error is its row's. The messages are byte-identical to
D.4c's (saved worlds' bad statements toast them).

## The anchor

A block's anchor is its own cell. The console's default is **the cell under the player's feet**:
`CmdScript::playerAnchor()` = `(floor(x), floor(feet − 0.01), floor(z))`, engine order. So `set 0 0 0
glass` replaces what you stand on, `tp 0 1 0` is "stay here", and a line typed standing on a command
block runs exactly as that block's script would (gate 2 proves it statement for statement). C.2's
`anchor` verb will re-point it.

## Running a line: `runLine` and jobs

`CmdScript::runLine(src, ax, ay, az, source, sink, ext)`:

1. Longer than 511 bytes (`MAX_LINE`, the `CMB1` limit, so any line of block verbs pastes into a block)
   → `RUN_LONG`. No statements → `RUN_EMPTY`.
2. **Any bad statement → nothing runs** (`RUN_BAD`): every error goes to the sink as
   `"N: message"` (N = statement number), then `"line not run"`. A block instead runs its good
   statements and toasts the bad ones (a saved world's scripts must not change meaning; a typed line
   that half-ran can't be undone).
3. No world in play → `RUN_NO_WORLD`.
4. A line containing `wait` while `MAX_JOBS` (4) jobs are waiting → `RUN_FULL`. A line with no `wait`
   always runs.
5. Otherwise it becomes an instance with `source` (`SRC_CONSOLE`/`SRC_HARNESS`), the anchor, the sink
   and a job id, and executes **at once** up to its first `wait`. Finished → `RUN_DONE`; still waiting
   → the job id (> 0), and the sink gets `[job N] waiting S s`.

Jobs live in the same instance list as blocks and are resumed by `CmdScript::update` (step 5b), but
they are **outside the 32-instance cap** (`liveCount()` counts blocks only) and have no green button.
They share `WORK_PER_FRAME`. One known slack: a line run before `update` in a frame uses that frame's
leftover budget, and `update` then resets it, so a frame in which a line was submitted can do up to 2×
the budget. A job is **depth 0**, so a `run` from the console starts the target at depth 1 and chains
8 deep, exactly as from a block (`gold 1; run 0 0 0` on a block gives 9 golds either way).

`jobs()` lists the waiting ones (id, wait left, pc, statement count, the line). `stop(N)` stops one,
`stop(STOP_JOBS)` every console job, `stop(STOP_ALL)` every job **and** every block instance; each
stopped job prints `[job N] stopped`. `reset()` (a world closing or warping) drops everything and
restarts job ids at 1. Runtime state is never saved.

## Output: the `Sink`

`CmdScript::Sink::line(kind, text)` with `OUT`/`ERR`/`JOB`. The sink must outlive its jobs. For a
console job:

- `say TEXT` toasts (as in a block) **and** prints `TEXT`.
- Refusals that a block toasts (rate-limited) print instead: *Command blocks can chain 8 deep*, *Too
  many command blocks running (32)* (from a job's `run`), plus *No command block at X Y Z* for a `run`
  with nothing there (a block says nothing).
- Cell edits are summarised as `N cells set, M refused` whenever the job yields (`wait`, the frame
  budget) or ends.
- A job that waited prints `[job N] done` when it ends.

A block instance has no sink: everything behaves exactly as in D.4c.

## The time hook

`OP_TIME` (`day/noon/night/midnight/time T`) calls `CmdScript::timeHook(t)`. The default is the region
sky repaint (`CmdScript.h`: Eden has no clock). A day/night cycle replaces the hook, and blocks and the
console change together.

## The registry: verbs and cvars (`Classes/Console.{h,mm}`, C.2)

`Console::ext()` is the one `CmdScript::Ext` the console passes to the parser. Two kinds of name register
in it, tried only after every built-in block verb (a block never sees them):

- **Verbs** (`addCommand(name, usage, help, flags, argKinds, fn)`): no `.` in the name, may not repeat a
  block verb or another registration (reported on stderr, then dropped). `CF_DIAG` verbs are absent
  (not refused) without `EDEN_DIAGNOSTICS`; `CF_CHEAT` verbs are refused unless a cvar
  `experiments.allow_cheats` exists and is on (none does in v1; no verb carries the flag); `CF_WORLD`
  needs a world; `CF_EDITS` is a marker for a future multiplayer client.
- **Cvars** (`addCvar(Cvar)`): dotted `group.key`, lower-cased for lookup (`keys.moveForward` also works as
  `keys.moveforward`). Kinds are Settings' (`KIND_TOGGLE/RANGE/ENUM`) plus `KIND_TEXT`; `apply` says when
  a change takes effect (`APPLY_LIVE`, `_NEXT_WORLD`, `_NEXT_CONVERT`, `_READONLY`).
  Values: a toggle takes `0 1 on off true false yes no`; a range takes a number (clamped by the setter,
  `clamped to N` is printed); an enum takes its label (any case, spaces optional: `saves.autosave 2min`)
  **or** an index, the label first (`video.render_radius 12` is the label); text takes the rest of the line.

A feature registers from its own init path with a plain call. **Settings and keybinds register
themselves** (`Console::init()` walks `eden_settings_*` and `eden_keybind_*`): every `kSettings[]` row is
`lower(group).key` (`video.fov`, `saves.autosave`), the rows `eden_settings_native_hidden` hides are
`CF_DIAG`, every non-hidden keybind is `keys.<action>` (value = the key's label: `Space`, `F9`, `` ` ``,
`none`). A new Settings row is a cvar with no console code. `eden_settings_apply(i)` (Settings_web.mm)
answers "when does it apply": by key, 1 for `render_radius`, 2 for `upgrade_256z`, else 0.
`world.name` (live: updates the node the next save reads **and**, with an `.emod` open, appends the
header through the open store), `world.gold` (0..999) are writable; `world.file/seed/height/type/signs/
cmdblocks` and `gen.hills/caves/trees/flowers/clouds/size/scale` are read-only in game (`gen.*` reads the
open world's `ClassicGen`, `n/a` for a non-Classic world); a set prints *Change terrain in World
Properties (world list)*. Not registered yet: `world.columns/created/version/home`.

Verbs: `help [X]`, `find TEXT`, `cvars [GROUP]`, `toggle V`, `reset V|GROUP|all` (`reset all` only
runs as `reset all yes`: there is no dialog from a typed line), `clear`, `history`, `echo TEXT`, `where`,
`anchor [me|look|X Y Z]`, `jobs`, `stop [N|all]`, `save`; diagnostics builds add `getblock`, `cmdtrace`,
`stats`. `jobs` leaves out the line that asked. `cells` and `quit` are not in the
registry until a `--live-cmds` session registers them (C.5, below); `cmdprobe` and `bench.save` stay legacy
`--live-cmds` verbs (`stats` covers the first).

**Harness (`--live-cmds`, C.5).** `native/src/seam/LiveCommands_native.mm` keeps its legacy verbs byte for byte
(`set` absolute and refusal-free, `tp`, `look`, `wait`, `rate`, `open`, `save`, `say`, `press`, `cells`,
`cmdprobe`, `cmdtrace`, `quit`; Q.5 added `photo`, the F2 screenshot) and adds `/<line>`: a console line run through `Console::run` with the anchor
fixed at `=0 =0 =0` (relative = absolute). Output is mirrored to stdout as `[live] <text>` (errors
`[live] ! <text>`). On the first `/` line it registers two harness-only `CF_DIAG` verbs, `cells X Y Z X2 Y2 Z2`
and `quit`. Gate 6: `port/tools/live-cmds-gate.sh` replays `port/tools/fixtures/live-cmds/gate.txt` under the
dummy video driver and diffs `expected.txt` (Linux, Windows, macOS legs). `DevConsole_web.mm` is harness
plumbing only: `eden_console_spawn`, `eden_console_world_stats` and the `eden_debug_bench_column_*` set are gone.

`Console::run(line)` records history, picks the anchor (`anchorIsFixed()`: the cell set by `anchor`, else
`playerAnchor()` re-read each line), and runs it through `CmdScript::runLine` with the console's sink.
Output goes to `Console::scrollback()` (200 lines, kinds `OUT/ERR/JOB/ECHO`) and to the optional
`setMirror` hook (the harness's stdout). The echo of the typed line is the screen's job.
`anchor look` and `where` read the cell under the screen centre (`FC_DESTROY`, as a build tap does).

**History** keeps the last 50 distinct-from-the-previous lines, persisted as one `console.history`
string (`0x1F`-joined: the native prefs store is one `key<TAB>value` line per entry). **Completion**
(`Console::complete(line)`) works on the statement after the last `;`: word 1 is a verb or cvar; later
words complete by the verb's argument kind (block and creature names, cvars, key labels) or a cvar's
values (`on/off`, enum labels, key labels). One match completes with a trailing space; several are listed
and the line takes their common prefix.

## The screen (`Classes/ConsoleMenu.{h,mm}`, C.3)

The player's way in. **Open**: the pause menu's *Console* button (closes the pause menu), or the
`console` keybind (default `` ` ``, native only, a `keys.console` cvar; opens from play, and closes
while the field is **not** focused, because a focused field owns every key, backquote included).
**Close**: Back, the key, or Escape twice (the first blurs the field, the TextField rule). Closing
returns to **play**, never to the pause menu. It exists only in play: `World::update` closes it if the
mode leaves PLAY. State lives on `Menu` (`menu->console`, `menu->showconsole`), like Settings.

**The world keeps running** (unlike `showsettings`, which freezes it). `World::update` calls
`console->update(etime)` first in the play step (a submitted line executes there, in the input phase,
like a build tap), then runs the whole play step. The screen claims every touch (`usage_id` 17), so
the HUD, joystick and player see none and every `hud->` flag re-derives to false. `eden_ui_wants_cursor`
is 1 while it is open (the mouse is released), and `eden_ui_captures_keys()` (Menu_web.mm) makes
`Input_native.cpp` drop every action key except `menu` and `console`, so a blurred field can never walk
the player. A creature can still hurt a player who is typing (no auto-pause; revisit at the T look).

**Full layout** (the shared `railFrame()` panel): titlebar *Back · Console · Collapse*; rail tiles
**Console** (`UIICON_TERMINAL`), **Variables** (`UIICON_SETTINGS`), **Help** (`UIICON_INFO`).
- *Console tab*: the scrollback (body face 15u, wrapped to the view, error lines red, echoed input lime,
  job lines grey; 200 lines; a row-snapped variable-height `ScrollView`), a tool row (the completion
  candidates as one line of text, the anchor, then **Tab / Up / Down** buttons: the touch spelling of
  Tab and the arrows) and the input: a 2-line `TextField` (511 bytes) + **Run** (= Return).
  The view follows the bottom unless the player scrolls up; lines that arrive meanwhile show a
  **New lines** pill. A tap on an echoed line puts it back in the field.
- *Variables tab*: one `TwoLineRow` per cvar (`name = value` over its hint, plus *(read-only)* /
  *(applies when a world opens)*); a tap puts `name ` in the input and returns to the Console tab. Values
  are re-read every frame the tab shows.
- *Help tab*: the block verbs (`CmdScript::verbs()`, marked *also works in command blocks*) and then the
  console verbs (`Console::commandAt`), a tap puts `verb ` in the input. It is the cmd.html reference, kept
  current by the same tables.

**Strip layout** (Collapse): the last three lines over a 1-line input + Run, with *Menu* (C.11: the collapsed button's label; *Collapse* when expanded) and *Back*,
docked to the **top** edge so the world shows below; the panel is 55% translucent (MP.14), and a fresh `openChat` opens in this layout. The choice is remembered for the session.

**Cost.** A scrollback line is one `Label` built the first frame it is shown and kept, keyed by
`Console::scrollbackSerial()` (lines ever printed: `scrollback()[i]` is serial `serial - size + i`), until
it falls out of the 200 kept; the cache is dropped when the text width or `u()` changes. The Variables
and Help rows (about 120 in all) are built the first time each tab is shown. **Deviations from the
plan**: no tappable completion chips (the candidates print as text; Tab/Up/Down are buttons), the
scrollback builds all its labels rather than only the visible ones (heights need them, `ScrollView` is
variable-height).

**The on-screen keyboard (C.4, 2026-10-16).** While the input is focused, `layout()` asks
`eden_text_input_keyboard_top()` (point space, y up; 0 = none, -1 = coming but not measured yet) where
the keyboard is. Over the Console tab: if the keyboard reaches into `railFrame()`'s panel, the panel is
rebuilt (`ConsoleMenu::frame`) from just above the keyboard to just under the screen top, **without the
rail** (the tabs come back when it goes; C.11: a *Tabs* chip beside *Back* hints at them, a tap blurs the field); the input is then the lowest thing above the keyboard and
re-announces its rect, so SDL's view shift has nothing to do (`keep_visible` is not used). If that panel
would show fewer than **4** scrollback lines (iPhone landscape), or the height is still -1, the
**auto strip** is used: the top-docked strip, no *Menu* button, dropping lines (to one) rather than
reach under the keyboard; it is not the player's collapse and ends with the keyboard. The field is a
**code field** (`TextField::setCodeMode`): text input starts through `eden_text_input_start_code()`
(no autocorrect, no autocapitalisation), and a hardware keyboard's **Tab / Up / Down** arrive as the
bytes `EDEN_TEXT_KEY_TAB/UP/DOWN` and do what the buttons do (`TextField::takeKey()`; a field not in code
mode drops them). Only the iOS observer reports a keyboard (`native/src/seam/IOSKeyboard_native.mm`; a
floating/split iPad keyboard reads 0); desktops and the harness answer 0 unless
`eden_native_debug_set_keyboard_top()` fakes one (`--ui-selftest`, the `console-keyboard[-tall]` shots).

Pitfall found building it: `Cvar::name/label/help/options` point into the registry record's
`std::string`s, and short strings live *inside* the string object, so the records must never move: the
registry is a `std::deque` (a `std::vector` of them dangled every short name after the first growth).
`--ui-selftest` asserts `cvarIndex(cvarAt(i)->name) == i` for every cvar.

## Player-facing text (C.9, C.10)

`native/src/seam/PlayerReadme_native.{h,cpp}` holds the player's `README.txt` (written into the documents directory
at startup by `eden_main_after_args`, never in a harness mode). Its first line is a version (`Emod README v1`): bump it
whenever the text changes; the file is rewritten only when that line differs, so a player's edits to a current file
stay. [console-reference.md](console-reference.md) is the commands reference; its verb and variable tables come from
`Console::referenceMarkdown()` via `eden_native --console-dump` and `port/tools/console-reference.py` (never edited by
hand). `--console-selftest` checks that every `` `backticked` `` command or variable in the README exists, that the write
is idempotent, and that the reference names every verb, console verb, variable and variable group
(`--console-reference=PATH`, default `../docs/console-reference.md`).

## Gates (`--console-selftest`)

The screen's gate is `--ui-selftest` (below the Settings/Properties blocks: the pause menu's seven buttons fit,
Console opens from the pause menu, Return runs a line and echoes it, Up/Down recall, Tab completes, the
Variables and Help tabs list every cvar/verb, Collapse/Run/Expand, Escape blurs then closes, the `console`
key opens it and a focused field owns it, Back closes) and `--shot` (`console`, `console-complete`,
`console-variables`, `console-help`, `console-strip`).

C.2 adds to the C.1 list below: **gate 3** every `kSettings` row is a cvar with the row's metadata and value
(hidden rows only under diagnostics); set/clamp/enum/toggle/reset; `render_radius` reports *applies when a
world opens*; `keys.jump` rebinds, the old key stops and the new one fires; `world.seed`/`gen.*`
refuse; `world.gold`; `world.name` round-trips through a save and a reload (the header read back with
`FileManager::getName`). **Gate 4** completion (`fi` -> `fill find fire`, blocks, enum labels, keys),
`help set` = the parser's usage, `find`, `cvars`, history cap/repeat/persistence. **Gate 5** `CF_CHEAT`
refusal, `CF_DIAG` presence matching the build, duplicate/dotted names dropped, a block cannot see the
registry. `--console-mutate=noreg` (the bridge off) must fail, and CI asserts that on Linux, Windows and
macOS (the iOS simulator leg only runs the pass).

### C.1 gates

World-free: the literal-script table shared with `--cmds-selftest` (`cmds_parser_cases`); **gate 1**,
`port/tools/fixtures/cmds/corpus.txt` (108 lines: every cmd.html example and recipe, every verb, the
boundaries, triggers, unknowns, `=` and stand-in registry lines), each parsed with both options and
classified B/b/c/x as annotated, field-compared where both accept; the verb table vs the parser's
errors; `runLine`'s refusals. In a fresh world in `--docs`: **gate 2**, 16 scripts each pressed on a
command block P and then typed with the player standing on P (cells of a 16×12×16 site, player
position, purse, health, creatures, sky, `say`'s toast must match, and the scripts must have changed
something); then jobs/`stop`/`=`/the sink/`OP_EXT`/the time hook. `--console-mutate=anchor`
(`testAnchorFeet`: the feet cell instead of the one under) must fail it, and CI asserts that on every
leg (Linux, Windows, macOS, the iOS simulator).
