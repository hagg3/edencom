# Conventions & Pitfalls (Port Layer)

> Written for the web build (retired 2026-10-13) and cut to what still applies to the native targets;
> the web-only rules (headless node driving, browser GL introspection, `EM_JS` macro commas, IDBFS) are
> at tag `web-port-final` ([web-port-archive](../../docs/web-port-archive.md)).

Read [`../../docs/conventions-and-pitfalls.md`](../../docs/conventions-and-pitfalls.md)
first (the engine conventions) — the
`(x, z, y)` argument order, storage layout, singleton pattern, typos-as-API, and on-disk
struct rules all apply unchanged. They now apply to *you* as well as to the engine:
since the never-edit-`Classes/` rule was retired (2026-07-25) you can be the one violating
them. This file only adds the port's *own* traps, learned the hard way across many passes
and recorded in full in `../../WORKING/archive/PORT-STATUS-2026-08-13.md`'s "distilled hard-won knowledge"
section — this is the durable summary of that.

## Editing engine code
Permitted since 2026-07-25; see [`../CLAUDE.md`](../CLAUDE.md) for the three rules that
replaced the blanket prohibition. In short: the **format/compat freeze is still absolute**
(no renumbering block types, no regenerating `colorTable`, no reordering or resizing the
raw-memcpy'd on-disk structs, no touching `CHUNK_SIZE` or the save-format height rules —
`T_SIZE` reaches no save file since B.5b-2); **platform
differences still belong in `seam`/`shim`**, not in `Classes/`; and the untouched import is
tagged `pristine-engine`, so `git diff pristine-engine -- Classes/` always shows the
divergence. Keep engine edits in their own commits, and update the matching `../../docs/`
topic file when you change the behaviour it describes.

Two engine hazards the root doc flags that now bite directly, because you can reach them:
the mesher `rebuild2()`'s counting pass and fill pass must agree exactly or you get heap
overruns (modify both or neither), and `Terrain::update`/`World::update` step order is
load-bearing (save-before-stream, edit-before-mesh, mesh-before-upload).

## Rules specific to this port

0. **Prefer the smallest lever that fits, but don't contort to avoid `Classes/`.** A link-time
   wrap or a seam file standing in for a one-line engine edit is the *worse* option — the
   indirection hides intent (the web build's `--wrap=` levers are gone, W.1). Seam/shim remain
   correct for anything platform-shaped.
1. **A seam replacement owes the side effects of what it replaced, not just its return value.**
   Grep the original for global assignments before writing a replacement. Bitten 4×: the
   `FileManager` ctor, `ShareUtil`, the screen-metric globals, `SettingsMenu::update`'s inline
   music toggle.
2. **Any `hud->` input flag is re-derived from touches every frame.** Never write the flag
   directly from host code — it gets silently overwritten next frame. Drive the HUD through
   synthetic touches (`eden_tap_hud_button_begin/end`). Bitten 3×.
3. **Measure the ABI/behaviour, don't reason about it.** A `clang -S -emit-llvm` dump, an
   `fprintf` probe in a real run, a `--*-selftest` mode, or a grep — never recall or extrapolate
   it. Every serious bug in this port was mis-diagnosed at least once by reasoning instead of
   measuring (point sprites, `BUILD_OGLES`, ivar layout, an "X-mirror" bug that was actually a
   V-flip).
4. **On the GNU ObjC ABI (Linux/Windows), a send that crashes in dispatch almost always means a
   missing `@implementation`** (it links fine and dispatches into nothing), not a bad prototype —
   check that first ([objc-runtime.md](objc-runtime.md)).
5. **Headless is the fast loop for logic questions; a window or a human is for pixels and feel.**
   `eden_native --headless …` with a fresh `--docs=` (port/CLAUDE.md "Headless driving"). Before
   trusting a screenshot, prove the loop is stepping.
6. **Settings and keybinds each live in exactly one place, both C tables:**
   `src/seam/Settings_web.mm`'s `kSettings[]` and `kKeybinds[]` (a binding is a USB HID usage ID —
   the number SDL3 scancodes use). The GL Settings screen renders both; add a row there, nowhere
   else. See [ui.md](ui.md).

## Where a root-doc rule needs a port amendment
- **Threading**: root says "all GL work on the main thread; the only other thread is the
  world-load pthread." Still true for engine code. The port adds one detached thread per HTTP
  request (`Net_native.cpp`) and SDL's audio device thread; neither runs engine code. `MeshPool`
  and worker column decode are compiled out (no build defines `EDEN_THREADED`). The web build's
  single-threaded default (`build-st`) is history.
- **On-disk structs**: still raw-memcpy'd, still must never be reordered/resized — and a world is
  now an `.emod` whose records carry them ([emod-file-format.md](../../docs/emod-file-format.md)).
