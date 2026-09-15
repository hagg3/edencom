# Conventions & Pitfalls (Web Port)

Read [`../../docs/conventions-and-pitfalls.md`](../../docs/conventions-and-pitfalls.md)
first (legacy iOS documentation, but the conventions themselves are current) — the
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
raw-memcpy'd on-disk structs, no touching `T_SIZE`/`T_HEIGHT`/`CHUNK_SIZE`); **platform
differences still belong in `seam`/`shim`**, not in `Classes/`; and the untouched import is
tagged `pristine-engine`, so `git diff pristine-engine -- Classes/` always shows the
divergence. Keep engine edits in their own commits, and update the matching `../../docs/`
topic file when you change the behaviour it describes.

Two engine hazards the root doc flags that now bite directly, because you can reach them:
the mesher `rebuild2()`'s counting pass and fill pass must agree exactly or you get heap
overruns (modify both or neither), and `Terrain::update`/`World::update` step order is
load-bearing (save-before-stream, edit-before-mesh, mesh-before-upload).

## Rules specific to this port

0. **Prefer the smallest lever that fits, but don't contort to avoid `Classes/`.** A
   `--wrap=` on a mangled symbol standing in for a one-line engine edit is now the *worse*
   option, not the disciplined one — mangled names are fragile and the indirection hides
   intent. Wrap/seam/shim remain correct for anything browser-shaped.
1. **A seam/wrap replacement owes the side effects of what it replaced, not just its
   return value.** Grep the original for global assignments before writing a
   replacement. Bitten this port 4×: the `FileManager` ctor, `ShareUtil`, the
   screen-metric globals, `SettingsMenu::update`'s inline music toggle.
2. **Any `hud->` input flag is re-derived from touches every frame.** Never write the
   flag directly from web-side code — it gets silently overwritten next frame. Drive
   the HUD through synthetic touches instead (`eden_tap_hud_button_begin/end`-style).
   Bitten this port 3×.
3. **Measure the ABI/behaviour, don't reason about it.** A real `em++ -S -emit-llvm`
   dump, an `fprintf`/`SAFE_HEAP` probe in a real run, or a grep — never recall or
   extrapolate it. Every serious bug in this port was mis-diagnosed at least once by
   reasoning instead of measuring (point sprites, `BUILD_OGLES`, ivar layout, an
   "X-mirror" bug that was actually a V-flip).
4. **`RuntimeError: function signature mismatch`** under the GNU ObjC ABI almost
   always means a missing `@implementation` (bad receiver), not a bad prototype —
   check that first.
5. **Headless (`node build-st/eden.js`) is the fast iteration loop for logic
   questions; the browser is for pixels/feel only.** The non-`MODULARIZE` build's
   `Module` is not visible to a bare `require()` caller (var-shadowing/TDZ-like
   behaviour) — drive it with `vm.runInThisContext(src, {filename})` in global scope.
   See [execution-flow.md](execution-flow.md).
6. **Chrome-extension GL introspection via `javascript_tool` does not work.**
   `getContext('webgl2')` hands back a wrapper the page's own draw loop ignores, and
   calling it again can kill the live context while screenshots keep showing a stale
   frame. For draw-time state, add an `fprintf` in the shim and rebuild. Before
   trusting any screenshot, prove the loop is actually stepping (hold a move input,
   sample `eden_debug_player_state().pos` twice).
7. **Settings/keybinds each live in exactly one place.** Engine-backed prefs are one
   C table (`src/seam/Settings_web.mm`'s `kSettings[]`) rendered generically by
   `public/eden-settings.js` — add a setting in C, never in JS. Key bindings are the
   deliberate exception: a JS-owned `localStorage` blob (`window.EdenKeybinds`),
   because the C settings model stores floats only. See [ui.md](ui.md).
8. **`EM_ASM`/`EM_JS` macro bodies can't contain a top-level comma inside a `[...]`
   literal** — the C preprocessor's paren-only balance tracking mis-splits the macro
   argument. Write such JS procedurally (build the array with statements) instead of
   as one literal.

## Where a root-doc rule needs a web amendment
- **Threading**: root says "all GL work on the main thread; the only other thread is
  the world-load pthread." On web, the default build (`build-st`) is single-threaded end
  to end — there is no separate load thread at all (`src/seam/pthread_sync_web.c` runs
  the load routine inline). `-DEDEN_THREADED=ON` (`build-thr`) restores exactly root's
  arrangement — one real world-load thread, everything else including all GL on the main
  thread — and needs a cross-origin-isolated page for its `SharedArrayBuffer` memory. It
  does **not** use OffscreenCanvas; that plan was dropped in pass 63. See
  [build-and-toolchain.md](build-and-toolchain.md).
- **On-disk structs**: still raw-memcpy'd, still must never be reordered/resized —
  but now also flowing through IDBFS persistence, not just the native filesystem. See
  [save-load.md](save-load.md).
