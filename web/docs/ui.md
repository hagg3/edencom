# UI Architecture (Web Port)

Read [`../../docs/ui.md`](../../docs/ui.md) first — the custom-GL HUD/menu system
(`Hud.mm`/`Menu.mm`) is still compiled and still authoritative for state and side
effects. What's different on web: several *screens* are replaced by DOM overlays that
drive the same engine state via synthetic input, rather than by editing `Classes/`.

**For how those overlays *look*, read [design-system.md](design-system.md)** — tokens,
components, the scale unit, the accessibility contract, and the deviations from the
Figma-derived kit. This file covers what each screen *does*; that one covers how
anything in the DOM is built. Open `public/eden-ui-specimen.html` to see every
component at once without booting the engine.

## Settings panel (`public/eden-settings.js` + `src/seam/Settings_web.mm`)
`SettingsMenu`'s data model (`properties[]`, `load()`, `save()`, `getNewWorldName`)
stays authoritative; only its `update()`/`render()` become no-ops, via
`-Wl,--wrap=_ZN12SettingsMenu6updateEf` / `renderEv` — possible because both are
called from `Menu.mm`, a different translation unit, so the wrap can intercept the
cross-TU call without touching `SettingsMenu.mm` itself.

One C table, `kSettings[]` (`src/seam/Settings_web.mm`), is the single source of
truth for what the DOM panel shows — `eden_settings_schema()` exposes it, and
`public/eden-settings.js` renders it generically. **Add a setting in C, never in
JS.** Currently 22 settings across 6 groups — `Gameplay` (health, autojump,
creatures), `Audio` (music, sound, music volume, effects volume), `Controls` (input
mode, hold-to-act, mouse sensitivity ×2, invert-look), `Video` (FOV, display mode,
render scale, max device-pixel-ratio), `Interface` (crosshair, block preview), and a
dedicated **`Experiments`** group for opt-in/non-stock movement toggles: fly mode,
frame-rate normalize (`fps_normalize`), advanced movement/bhop (`advanced_movement`),
and crouch mode (`crouch`, gates `CROUCH_ENABLED`, see
[player-input-camera.md](player-input-camera.md)). `fly`, `fps_normalize`, and
`advanced_movement` previously lived under `Gameplay`/`Controls`; only their `group`
string moved to `Experiments`, their engine defaults and behavior are unchanged.

**Trap**: `SettingsMenu::load()` hard-codes `player->invertcam = FALSE` and
`hud->use_joystick = TRUE` on **every** load (`Classes/SettingsMenu.mm`) — this is
engine behavior, not a bug to fix. The port's own `eden_apply_port_settings()` must
re-run after every `save()`/`load()` or these two values silently revert.

Keybinds are the **one deliberate exception** to "settings live in the C table":
`window.EdenKeybinds`, a JS-owned `localStorage` blob mapping action → physical key
code, because the C settings model only stores floats. See
[conventions-and-pitfalls.md](conventions-and-pitfalls.md) #7.

**Unified surface (audit row 20/G2, 2026-08-04).** The storage split above is real and stays —
but the panel's UI was already a single shell before this pass, contrary to the audit's pass-59
note: `Keys` (keybinds) and `Storage` are tabs in the same rail as every schema-driven group. The
actual gap was that there was no *shared* reset — only the Keys tab had one. Added
`eden_settings_reset_all()` (loops `kSettings[]`, resets each row via the existing
`eden_settings_set`) and a `Reset` tab (`renderResetBody` in `eden-settings.js`) whose one
confirm-guarded button calls that plus `window.EdenKeybinds.resetDefaults()`, then re-renders —
one reset, for both halves, in the tab list every other setting already lives in.

`NSUserDefaults` itself is localStorage-backed
(`src/shim/foundation/NSUserDefaults.mm`) — NSNumber/NSString only, namespaced under
`eden.prefs.`, guarded so headless (`node eden.js`, no `localStorage`) degrades to an
in-memory default.

## In-game menu (`public/eden-pausemenu.js`)
Replaces `Hud::renderMenuScreen`'s 4-icon GL screen (Resume/Save/Warp Home/Take
Photo/Settings/Quit) with a DOM overlay. No `--wrap` here — the original screen is
inline inside `Hud::update`/`render`, too intra-TU to intercept piecemeal. Instead the
overlay polls `hud->inmenu` (`eden_hud_in_menu()`) and drives every action as a
synthetic tap on the real button rects (`eden_tap_hud_button_begin/end`, with cases
added for save/warp-home/photo/quit) — same "drive it, don't write flags" pattern as
[player-input-camera.md](player-input-camera.md).

**It is titled "Menu", not "Paused"** — opening it does not pause anything. `hud->inmenu`
only gates input and swaps what `Hud::render` draws; `World::update` keeps running
underneath, so creatures move, fire spreads and the sun keeps travelling while it is up.

**Exactly one in-game menu is ever on screen, and `legacy_menu` picks which.** This used to
be true by accident: the DOM panel was a full-canvas opaque overlay, so the GL panel
rendering underneath was simply never visible. Once the panel shrank to fit (below) the
four GL icons showed through the scrim behind it, so it is now explicit and symmetric:

- `legacy_menu` **off** (default) — `Hud::renderMenuScreen` is suppressed via
  `eden_hud_draw_menu_screen_hook`, a hook added to `Classes/Hud.h` whose default `NULL`
  keeps stock behaviour, installed from a static ctor in `Menu_web.mm`. The DOM panel shows.
- `legacy_menu` **on** — the hook returns true, the GL panel draws, and
  `eden-pausemenu.js`'s `tick()` keeps the DOM panel closed (it reads the same flag through
  a new `eden_legacy_ui_active()` export). Checked every tick, so toggling the setting takes
  effect immediately, including closing a panel that is already up.

Why a hook and not a `--wrap`: the `renderMenuScreen()` call is **intra-TU** (both it and
`Hud::render` live in `Hud.mm`), so the compiler resolves it directly and the linker never
sees a symbol to wrap. Why a hook and not a plain `bool` on `Hud`: then there is no flag for
anyone to keep in sync — the answer is recomputed at the one moment it is needed.

**"Move Controls" (audit row 17/G1, touch profile only).** The joystick pad
(`Classes/Joystick.mm`'s `padbounds`) was not repositionable — a grep for `draggable`/`drag`
across the port found nothing, and neither did a re-check confirming the same for this pass.
Fixed as an explicit, opt-in mode rather than an ambiguous long-press during normal play: the
button (visible only when `eden_effective_input_is_touch()`) hides this panel, calls
`window.EdenJoystickCustomize.start()` (`eden-input.js`), and shows a floating "Done" button.
While active, `Classes/Joystick.mm`'s `joystickCustomizeMode` flag makes `Joystick::update`
no-op entirely (see player-input-camera.md), so there is no real joystick input to fight; the
SAME touch/mouse pipeline `eden-input.js` already has for gameplay is reused to drag the pad
(`toEnginePoint()`, already there for touch->engine coordinate mapping), clamped to stay
on-screen against the live `ENGINE_WIDTH`/`HEIGHT`. The result persists to
`localStorage['eden.prefs.joystick_pos']` (same `eden.prefs.` family as the hotbar/keybinds) and
is restored on boot once `eden_settings_loaded()` is true (`eden-st.html`'s `trackCursorNeed`,
same gate `applyDisplayMode()`'s own restore uses) via `eden_joystick_set_origin`
(`web/src/seam/Input_web.mm`). Mouse-driven dragging works too (desktop testing without a phone),
gated the same way as the touch path.

**Live-browser bug (found and fixed 2026-08-04):** `Joystick::render()` draws two rects —
`padbounds` (the translucent ring) and `joystick_pos` (the solid knob). `eden_joystick_set_origin`
originally moved only `padbounds`. `joystick_pos` is only ever reset to `default_pos`
(`Classes/Joystick.mm`'s ctor: a fixed rect that happens to equal `padbounds`'s original (20,20)
because both start the same size, not because anything keeps them linked), and `Joystick::update`'s
"no touch active" branch re-snaps it to that stale `default_pos` every frame — including after
"Done", since normal play immediately starts calling `update()` again. Net effect: drag the pad,
and the knob stays glued to the OLD spot forever, permanently split from the ring it's supposed to
sit inside. `eden_joystick_set_origin` now moves `padbounds`, `default_pos`, AND `joystick_pos`
together, restoring the ctor's implicit invariant. This class of bug — something that only shows up
as two controls silently drifting apart on screen — is exactly why this feature's audit row kept
saying "needs a live-browser pass"; headless/static checks had no way to catch it.

Layout: a shrink-to-fit dialog (`.eden-window--fit`) whose width is set by its content
rather than the 460u dialog default, with left-aligned icon+label rows
(`.eden-stack--left`) laid out in **two columns** (`.eden-stack--grid`) — six full-width
rows made a tall thin panel whose labels hugged the left of a lot of dead width. There is
no title-bar close button — Resume is the first row, the scrim dismisses, and Escape
resumes; a fourth affordance for the same action was noise, and it was the one thing
forcing the title bar wider than the content. Resume keeps the vector `play` glyph;
Save/Warp Home/Take Photo/Settings/Quit carry the engine's own `media/ui` icon art via
`button({ iconImg })` (see design-system.md "Iconography").

The old `max-height:480px` reflow (pass 42, a 2x2 grid to rescue six stacked rows on a
phone-landscape viewport) is **gone** — there is no six-row stack any more, and it was also
catching the prose+buttons confirm dialogs, which never wanted two columns. A
`max-width:420px` query drops the grid back to one column, since two ~180px columns don't
fit a portrait phone. The settings panel still tightens its rhythm and hit targets at
`max-height:480px`, keyed on viewport *height* — the input stays touch, only the rhythm
changes. **That query used to `display:none` the settings rows'
description lines**; it no longer does. Dropping them was wrong in practice — several
settings are only intelligible from the hint, and a phone is exactly where you cannot
hover a tooltip to recover it. They now just shrink (with a 12px floor, since `--u` has
already bottomed out at 1 by this breakpoint) and the content box scrolls, which is the
cheaper failure. Live-verified at 1000x383 CSS px with a real resized window.

## Loading/progress overlay (`public/eden-loading.js`)
Perf-audit row 11 ("minute-plus black screen on mobile"). A DOM overlay (title, a
progress bar, byte-count sublabel) shown from the moment `eden-st.html`'s inline
script starts running — before `Module` even exists — until `moduleReady` is true
*and* a few dozen rAF frames have passed (reuses the existing `watchFrames` first-frame
heuristic, but now gates on `moduleReady` too — see the comment at its call site;
`watchFrames(120)` alone fires on a fixed rAF-frame count from page load, independent
of whether the engine has actually finished booting, which would hide the overlay
early on a slow/cold load). Doesn't wrap `window.fetch` (rejected — too easy to
subtly break Emscripten's own wasm/data fetches for a page that only needs progress on
two specific large assets); instead:
- `eden.data` (the small UI/HUD/atlas asset package): polls Emscripten's own
  `Module.dataFileDownloads['eden.data']`, which its stock generated loader already
  populates byte-for-byte — no project code needed on that side.
- `Eden.eden` (the ~52 MB default-world map, see [save-load.md](save-load.md) and
  `src/seam/js/eden_default_world.pre.js`): that seam file's browser fetch path now
  streams the response body (`ReadableStream`, falls back to a plain `arrayBuffer()`
  read if unsupported) and reports through `window.EdenLoading.setEdenFileProgress`.

The two byte-tracked spans are weighted by total size and mapped into the middle 90%
of the bar (5% head/tail reserved for wasm compile and post-download `main()`/
`World::World()` startup, neither of which has a useful byte-level signal). Doesn't
touch the existing `#eden-status` diagnostic line (12px monospace, top-left) — that
stays as-is for debugging a stuck/failed boot per RESUME-HERE's "Watch out for"
section. Live-verified (pass 43): the indeterminate "Starting…" state renders on a
real cold load; the determinate byte-progress labels and clean removal on
`markReady()` were verified via direct API calls (a same-origin dev server serves
this project's assets too fast locally to observe a real multi-second download).

## Load-failure recovery dialog (`public/eden-loaderror.js`)
A DOM "this world could not be loaded" panel, same shape as the pause menu (opaque
overlay, no engine state of its own) — polls `eden_load_failed()`
(`web/src/seam/LoadFailure_web.mm`) once a frame and offers Restore-`.bak`-and-reload
or Back-to-menu. See [save-load.md](save-load.md) for the mechanism and pass 42's two
fixes (a failed load previously still crashed the engine a frame or two after this
dialog appeared, and the Restore button previously destroyed the very backup it was
meant to restore) — both now fixed and live-verified end-to-end.

**Tall-world variant (ROADMAP Phase M / M5.3).** When the load-failure *reason* is the exact token
`TALL_WORLD_LOW_MEM` — `World::loadWorld`'s pre-flight refusal of a 256z world on a
low-memory-flagged device, not a corrupt file — `buildTallWorld()` renders a different panel
("World needs more memory", no Restore button, points at Settings → Storage → "Convert to 64z").

## Alert/dialog seams (`src/seam/seam_link_stubs.mm`)
Real DOM dialogs (`EM_ASM`) replace what used to be auto-answer stubs. (The *native* build
answers the same `showAlert*` functions with an engine-drawn GL modal, `Classes/GLDialog.{h,mm}`,
added in Phase N Stage 2.5 — see `../../docs/ui.md`. Web is unchanged: it never calls
`GLDialog::show()`, so that primitive is inert here.)
- World-type dialog (Flat/Normal) — falls back to auto-answering "Normal" when
  `document` doesn't exist (headless).
- Warp-home confirm dialog — button order transcribed literally from `Alert.mm`'s
  `-alertView:clickedButtonAtIndex:` delegate semantics, factored through a generic
  `eden_js_alert_dialog()` helper reused for other confirms.

**Both overlays must stack above `--eden-z-menu` (25) and `--eden-z-panel` (30)** — they use
`var(--eden-z-alert,40)`. They were a hard-coded `z-index:20` until 2026-08-06, i.e. *under* the
DOM menu, and for the world-type dialog that was not cosmetic but a hang: `Menu::render()`'s
`loading` ladder parks at 3 until the dialog is answered, so an invisible, unclickable modal
means the game sits on "Loading world… 0%" forever with no way out. Reported by the user,
reproduced in real Safari (`document.elementFromPoint` over both buttons returned the menu's
`DIV.eden-stack`, not the buttons), fixed by moving both onto the design system's z-scale.
**Anything new that parks engine state on a DOM answer belongs on that scale too** — never a
literal z-index.

`Alert.mm` itself stays seam-excluded (see [networking.md](networking.md)) — its
delegate *semantics*, not its implementation, are what's preserved.

## Storage tab
Lives in the same settings-panel UI; see [save-load.md](save-load.md) for the
mechanism.

## Passing data across the JS/wasm boundary
This port deliberately has no `_malloc`/`_free` on the wasm export list, so any
JS-side API that would otherwise need to pass a string or buffer *into* wasm is
designed to be **index-based** instead (e.g. storage-tab world deletion by row
index). Keep this pattern for new JS↔wasm settings/storage calls rather than adding
malloc/free exports.

## Port-invented UI (no engine equivalent)
A curated 9-block hotbar strip (backed by the engine's real block-picker data, but a
port-invented curated UI concept — the engine itself only has a scrolling grid) and a
DOM crosshair. See [player-input-camera.md](player-input-camera.md).

## Main menu / Load World / New World (`public/eden-menu.js` + `src/seam/Menu_web.mm`)
The three Figma mockup screens, rebuilt in the DOM as an **opaque full-viewport overlay
on top of the still-running GL menu** — the same "drive it, don't replace it" pattern as
the pause menu, and for a sharper reason: `Menu.mm`'s world-load state machine lives
inside **`Menu::render()`** (the `loading` ladder 1 → 2 → *(worldExists? 4 : alert + 3 →
4)* → `World::loadWorld`), so `--wrap`ping render away would silently break loading a
world. The overlay eats pointer input before it reaches the canvas, which is what stops
the two menus fighting.

`Menu_web.mm` is **not** a seam replacement (`Menu.mm` still compiles and runs); it only
exposes the engine's state and offers the same transitions the GL touch handler
performs: `eden_menu_active/loading/load_percent`, `world_count/name/file`,
`select/play/create_world/delete_at`, `set_pending_world_type`, `open_settings`. All
index-based, per the boundary convention below. The one string that has to travel
*into* wasm — a new world's name — goes through `eden_menu_name_buffer()`, a static
buffer JS writes UTF-8 into, rather than adding `_malloc`/`_free` to the export list.
The name field filters to `A-Za-z0-9' ` live on input (and again defensively before the
buffer write), matching the character set `ShareMenu::keyTyped` has always enforced on
iOS (`isalnum(c)||c==' '||c=='\''`) and that `tools/eden2/UploadMap2.java` enforces
server-side on upload — without it, a locally-created world can carry a name (e.g.
non-Latin scripts) that the shared-world service silently mangles on share.

Two behaviours worth knowing:
- **World names are filtered to `A-Z a-z 0-9 ' ` and space**, live as you type and again at the
  wasm boundary. That is the character set `tools/eden2/UploadMap2.java` accepts; anything else is
  silently stripped on upload, so a name that survives creation but not sharing is worse than one
  the field never let you type.
- **World type is asked up front.** The engine raises its Flat/Normal modal from inside
  the load ladder; the New World screen asks with a generator rail instead and parks the
  answer, which `showAlertWorldType()` (`seam_link_stubs.mm`) consumes. Falls back to the
  real dialog whenever nothing is pending, so it is additive. The rail's two live tabs are
  the engine's only two generators (`FileManager::genflat`) and the window title names the
  one selected — "New Flat World" / "New Normal World" — so the screen always says which
  world it is about to make; the third tab is `disabled`, not a placeholder.
- **Delete actually works now.** The GL path routes through `showAlertDeleteConfirm()`,
  which the port implements as a deliberate no-op (not confirming a destructive prompt is
  the safe default with no dialog), so the engine's own Delete button has never deleted
  anything on web. The DOM screen confirms in the DOM, then calls `a_deleteConfirm()`.
- **Height format is asked the same way, up front** (2026-08-26, 256z Stage 3 item 4). "Legacy
  64z" / "New Dawn 256z" is a real segmented control now (it used to have a `placeholder` 256z
  button, "not implemented in this build"). Same one-shot parking pattern as world type:
  `eden_menu_set_pending_world_height` (`Menu_web.mm`) is consumed by
  `FileManager::probeWorldHeight` — not a seam file, since that function already decides a
  not-yet-existing world's height — the moment the created world is actually played. 64z stays
  the default; nothing sets 256 unless this screen explicitly picked it.

The **`legacy_menu`** setting (Interface group, labelled "Legacy UI") turns the overlay off,
revealing the original 2010 GL menu underneath, fully functional. For the MAIN menu "legacy"
is literally "stop drawing the overlay", so there is no second code path to keep alive. The
in-game menu needs one extra step in the other direction — its GL panel is suppressed by
default and this flag re-enables it; see "In-game menu" above.

**Trap (fixed): global game hotkeys ate keystrokes typed into any DOM text field.**
`eden-st.html`'s `window` `keydown`/`keyup` listeners drive movement/action keys off
`e.code` with no regard for what has focus, so typing into the New World name input
used to also move/jump/toggle-fullscreen/open-Settings/etc. the moment a typed letter
matched a keybinding (`KeyE`/`KeyB`/`KeyL`/`KeyO`/**`Space`**, ...) — `Space` is the
worst of these since it also `preventDefault()`s, so the character never reached the
field at all. Both listeners now bail out via `isTypingTarget(document.activeElement)`
when an `<input>`/`<textarea>`/`contentEditable` element has focus. Any future DOM
text field inherits this for free; no per-field opt-in needed.

## The engine's retina/quality swap is ignored on purpose (audit row 22 / B7)
`World::update()` returns a bool meaning "swap graphics quality"; on iOS
`EdenViewController.mm` answered it by flipping `IS_IPAD`/`IS_RETINA`/`SCALE_WIDTH`/
`SCALE_HEIGHT` and recreating the framebuffer at the new density. **This port
deliberately does neither**, and `EdenViewController_web::drawFrame()` carries the full
reasoning. The short version, because it is the thing to understand before touching any
UI layout here:

> **`IS_IPAD`/`IS_RETINA`/`SCALE_*` are this port's layout coordinate system, not its
> resolution.** The engine lays every HUD and menu element out in a POINT space at 2×; the real
> drawable is decoupled entirely, via `applyDrawableSize()` (CSS box × `min(devicePixelRatio,
> dpr_cap)` × `render_scale`). Flipping those globals does not make pixels cheaper — it
> halves the UI's own layout math underneath an unchanged surface.

The port already exposes the *intent* ("cheaper pixels") as two real settings,
`render_scale` and `dpr_cap`. The branch used to flip the globals without recreating
anything, which was the worst of both worlds; it is now an explicit no-op that announces
itself once under `EDEN_DIAGNOSTICS` if it ever becomes reachable. It is unreachable
today — `World::update` only requests a swap when `!bestGraphics`, and nothing in this
port ever sets `LOW_MEM_DEVICE`/`LOW_GRAPHICS`, while `IS_WIDESCREEN` (always true for any point
space this port can derive) forces `bestGraphics` back on regardless.

## The point space: derived, not pinned (audit rows D1 + D4)
Until 2026-07-31 that point space was pinned at 568×320 in `EAGLView_web`'s
`establishScreenMetrics`, which is what made the HUD *enormous* on a desktop monitor: a 45-point
button drawn into a 2560-wide window came out 4.5× its design size. The point space is now derived,
and the whole of that derivation — plus the two-profile concept it hangs off — lives in
[`src/seam/DisplayProfile_web.mm`](../src/seam/DisplayProfile_web.mm). **Read that file's header
before changing anything about layout, sizing or the settings rows below.** The summary:

```
SCREEN_HEIGHT = 640 * 100 / ui_scale_pct        # a user/profile choice: UI DENSITY
SCREEN_WIDTH  = round_even(SCREEN_HEIGHT * aspect)   # the real window's aspect (clamped 1.2–2.4)
P_ASPECT_RATIO = SCREEN_WIDTH / SCREEN_HEIGHT        # ALWAYS derived — see below
```

- **Two profiles, `desktop` and `touch`**, auto-detected from the existing input-mode arbitration
  (`eden_effective_input_is_touch()` — one detector, one user override, for both concepts). A
  profile is a row of *defaults*, never a code path: UI scale, layout aspect, fps cap, DPR cap,
  render scale, and whether the on-screen joystick/jump chrome is drawn.
- **Touch defaults are 200% + Adaptive, with a classic-WIDTH floor** (changed 2026-09-07; they were
  200% + `Classic 16:9` until then). On a 16:9 screen that is still 568×320 to the point, so a
  player on the device the layout was drawn for sees a bit-identical HUD. The pin was the audit's
  "keep the pinned profile as the default until it's verified on a real phone" mitigation, and the
  verification is what retired it: a real iPad Air 2 played the native build and reported black
  bars top and bottom, because a 4:3 screen letterboxed to a pinned 16:9 (STATUS §2.0.12, bug 4).
- **The classic-width floor is what the pin was standing in for.** Every HUD and menu rect in
  `Classes/` is an absolute number of points laid out against a 568-point width, so a *narrower*
  point space runs the UI off the right edge (and `IS_WIDESCREEN`, ~10 branches, flips below 480).
  The floor says the derived point space is never narrower than 568 — raising `pointH` when the
  aspect would otherwise produce less, since height is derived first and width follows the aspect.
  On 4:3 that lifts a 200% layout from 426×320 to **568×426**: same UI width as the classic layout,
  the extra points spent on world instead of on bars. It is the sibling of the density floor below
  (never *denser* than the device the art was drawn for) in the other axis, and it runs *after* it:
  the density cap is a comfort heuristic, this is a correctness floor, so this one wins.
- **Desktop defaults are 125% + Adaptive**: half-size UI, and no letterboxing — a wider window shows
  more world (vertical FOV is fixed, so a wider aspect widens the horizontal field) instead of the
  same world with bigger buttons.
- **A density floor keeps small windows usable.** `ui_scale` alone gives a fixed point space, i.e. a
  UI that is a fixed *fraction* of the canvas — so shrinking the window shrinks every HUD icon with
  it. That is right on a monitor and wrong the moment the window gets phone-shaped, and nothing
  about resizing a mouse-driven window flips the touch profile, so the desktop 512-point space stays
  in force and the 45-point buttons keep shrinking past ~35 CSS px (reported from live play,
  2026-07-31). The floor is **one engine point is never smaller than one CSS pixel** — not an
  arbitrary number: on the iPhone 5 the viewport was 568×320 CSS pixels and the point space was
  568×320, so UIKit points *are* CSS pixels and this says "never denser than the device the art was
  drawn for". It is applied against the letterboxed canvas box, not the raw viewport, and it makes a
  phone-shaped window degrade toward the classic layout instead of a miniature of the desktop one.
- **Two settings rows expose it**, both defaulting to `Auto`: `ui_scale`
  (`Auto,100%,125%,150%,200%`) and `display_layout` (`Auto,Classic 16:9,Adaptive`). `Auto` is not
  the same as picking the profile's value by hand — it re-resolves if the input mode changes.
  Deliberately *not* seeded by the profile-defaults writer for that reason; `dpr_cap`/`fps_cap`/
  `render_scale`, which have no `Auto` option, still are.
- <a name="low-memory-overlay"></a>**A third row, `low-mem`, is an *overlay*, not an input profile
  (ROADMAP Phase M / M5.2).** `eden_active_profile()` never returns it — the page calls
  `eden_set_low_memory(1)` (before `eden_settings_init`) when `navigator.deviceMemory ≤ 4`, when a
  prior threaded-build load-failure downgrade is remembered, or on `?lowmem=1`. While set,
  Settings_web.mm's profile-default seeder takes `dpr_cap` / `render_scale` (and `fps_cap` if the
  row sets it) from `kProfiles[EDEN_PROFILE_LOWMEM]` — 1× pixel ratio, 75% render scale, 45 fps —
  instead of the desktop/touch row underneath; `ui_scale` / layout / control chrome are unaffected.
  Still a *default*: a row the player has touched wins. It also makes `World::loadWorld` refuse a
  256z world (see [world-and-terrain.md](world-and-terrain.md), and the load-failure dialog's
  tall-world variant below).
- **A stock bug not reproduced:** `Classes/EAGLView.mm:138-143` only recomputes `P_ASPECT_RATIO`
  inside `if(IS_WIDESCREEN)`, leaving the other branch on an iPad 4:3 default that matched no live
  layout. This always derives it. (iPad's own 4:3 branch is *not* resurrected — it is commented out
  in the original and never drove a live layout on any device. See
  [`../../WORKING/archive/aspect-ratio-toggle-scope.md`](../../WORKING/archive/aspect-ratio-toggle-scope.md).)

Engine-side, this needed `Hud::layoutForScreen()` / `Menu::layoutForScreen()` (rect math lifted out
of the constructors so it can run more than once, and kept idempotent) and
`Input::screenMetricsChanged()` — see root [ui.md](../../docs/ui.md) and
[conventions-and-pitfalls.md](../../docs/conventions-and-pitfalls.md) for those. Page-side,
`applyDisplayMode()` hands the available box to `eden_display_set_viewport()` *first*, then
letterboxes the canvas to whatever aspect the engine chose — which is the box's own aspect in
Adaptive mode, so the letterbox is normally a no-op.

**Three invariants that will bite silently if broken:**
0. **Vertically-coupled UI must share an anchor edge.** Two pairs in `Classes/Hud.mm` were anchored
   to *opposite* edges and only agreed at 320 points — the picker card vs. its own swatch grid, and
   the mode-button column's proportional spread. Both shipped broken in the first cut of D1 and were
   caught by looking at the game, not by the suite. Root `docs/ui.md` has the detail; the lesson for
   anything added here is that "it lines up at 568×320" is now evidence of nothing.
1. **`GL_VIEWPORT` must report the point space × `SCALE_*`, never the real drawable.**
   `Util.mm`'s `findWorldCoords` scales a point-space tap by `SCALE_*` and unprojects it against
   whatever `GL_VIEWPORT` says, so the two have to agree. The GL shim's `kPickViewport` is that
   answer and `eden_gl_set_pick_viewport()` keeps it in step; answering the real drawable there is
   what once made mobile taps land left of the finger (perf-audit item #6). Wrong by a constant
   factor = invisible at the screen centre, worse toward the edges — a centre-of-screen smoke test
   cannot see it.
2. **Re-layout must be idempotent.** `Hud.mm`'s margins are file-static and *mutated* by the layout
   body; they are reset at the top of `layoutForScreen()` for exactly this reason.

`tools/headless-display-profile-test.js` pins both invariants, the stock 568×320 rects byte-exact,
the derivation, the clamps, and the profile's control-chrome field.

**Live-verified on desktop 2026-07-31** (menu, in-world HUD, mode column, picker card, window
resize down to phone proportions) — that pass is what found the two anchor bugs and the missing
density floor, none of which the headless suite could see. **Still owed: the touch profile on real
phone hardware.** It should be pixel-identical to the pre-D1 build; if it is not, that is a
regression, not a design question.

## Known gaps
- **World name display**: `Menu.mm` draws the "EDEN" wordmark, not the selected
  world's name — the actual name is drawn through a `statusbar` (custom-GL text, see
  root [ui.md](../../docs/ui.md)), which has no numeric debug-probe proxy. Moot for the
  DOM menu, which shows names directly; still true of the legacy GL menu.
- **`VKeyboard`** (world renaming / search text entry) has no web replacement yet —
  the original overlays a real `UITextField` via a UIKit method call. Currently
  seam-excluded with no replacement. The New World screen's DOM `<input>` covers world
  *naming* at creation; renaming an existing world still has no path.
- **Mockup features with no implementation** (see design-system.md "Placeholders and
  disabled controls"): Get Worlds (needs the edengame.net client), per-world Share and
  Info, and the New Dawn 256z height format (blocked by the `T_HEIGHT` format freeze) are
  focusable placeholders that explain themselves; the Biome generator tab is a plain
  `disabled` control, since "third world type, switched off" needs no explanation.
- No onboarding/controls-discovery overlay, and "reset settings" doesn't reset the
  JS-owned keybind blob. (Corrupt-save recovery UI now exists — see "Load-failure
  recovery dialog" above and [save-load.md](save-load.md); first paint is now the boot
  progress screen, not a bare status line.)

## `public/*.js` dependency graph (audit row I2)
Thirteen files, ~2.9 kLOC, loaded as plain `<script>` tags (no bundler, no `MODULARIZE`) sharing
`window` globals — deliberate, not an oversight: the non-`MODULARIZE` `eden.js` output and the
`vm.runInThisContext`-based headless harnesses (`tools/headless-*.js`) both depend on everything
living in global scope, so a module system would need its own headless-compatible shim before it
earned its keep. Load order in `eden-st.html` is therefore significant and is the actual dependency
order — a file may only assume an earlier `<script>`'s global already exists:

```
eden-icons.js  →  eden-ui.js  →  eden-assets.js  →  eden-loading.js  →  eden-opfs.js
   →  eden-storage.js  →  eden-settings.js  →  eden-pausemenu.js  →  eden-loaderror.js  →  eden-menu.js
   →  eden-gamepad.js  →  eden-console.js
```

(`eden-opfs-worker.js` is a fourteenth file but deliberately not in that list: it is loaded by
`eden-opfs.js` as a **dedicated Worker**, not a `<script>`, because `FileSystemSyncAccessHandle`
exists only in a worker realm. It shares no globals with the page and talks to it over one
request/response `postMessage` protocol.)

Each file publishes exactly one `window.Eden*` namespace object (assigned once, at the bottom of an
IIFE) and reads only the namespaces of files that loaded before it, plus the wasm boundary
(`Module.*`/`FS.*`) and the engine-owned `window.EdenKeybinds` blob `eden-st.html` itself installs.
`eden-ui.js`/`eden-icons.js`/`eden-assets.js` are the base layer every screen is built from and have
no dependency on each other in that order (icons/assets are pure data; `eden-ui.js` is the factory
library). `eden-gamepad.js` is the one leaf with no `Eden*` dependency at all — it only touches
`navigator.getGamepads()` and the bridge object `eden-st.html` hands it.

| File | Publishes | Reads (`Eden*`) | Reads (wasm/DOM boundary) |
|---|---|---|---|
| `eden-icons.js` | `EdenIcons` | — | — |
| `eden-ui.js` | `EdenUI` | `EdenAssets`, `EdenIcons` | — |
| `eden-assets.js` | `EdenAssets` | — | `Module.FS` |
| `eden-loading.js` | `EdenLoading` | `EdenUI` | `Module.dataFileDownloads` |
| `eden-opfs.js` | `EdenOPFS` | — | `FS.mount`/`getPath`/`lookupPath`, `MEMFS.mount`, `new Worker('eden-opfs-worker.js')` |
| `eden-storage.js` | `EdenStorage` | `EdenOPFS`, `EdenLoadError` (only inside a callback fired well after boot — see below) | `Module.preRun`, `FS.mount`/`mkdir`/`readFile`/`writeFile`/`syncfs`, `Module._eden_storage_list_worlds` |
| `eden-settings.js` | `EdenSettings` | `EdenUI`, `EdenStorage`, `EdenConsole` (feature-detect only), `EdenKeybinds` | `Module._eden_settings_schema` |
| `eden-pausemenu.js` | `EdenPauseMenu` | `EdenUI`, `EdenSettings`, `EdenAssets` | — |
| `eden-loaderror.js` | `EdenLoadError` | `EdenUI`, `EdenPauseMenu.tick` (to suspend it while the dialog is up) | `FS.syncfs` |
| `eden-menu.js` | `EdenMenu` | `EdenUI`, `EdenStorage`, `EdenAssets`, `EdenPauseMenu.tick` | — |
| `eden-gamepad.js` | `EdenGamepad` | — | — (Gamepad API + a bridge object passed in by `eden-st.html`) |
| `eden-console.js` | `EdenConsole` | `EdenUI` | — |
| `eden-st.html` | `EdenRenderer`, `EdenKeybinds` | all of the above | `Module`, `FS`, everything else |

**The one forward reference:** `eden-storage.js` (loaded 5th) calls `EdenLoadError.showStorageWarning`
(defined 8th) from inside `checkQuotaAndWarn()`/`flushNow()`'s error callbacks. This is safe *only*
because those callbacks fire asynchronously (a `syncfs` completion, a quota check after boot), by
which point every `<script>` tag on the page has already run — load order guarantees definition
order, not callback-firing order. Anything that instead calls a later file's global **synchronously**
during its own top-level IIFE execution would break. If you add a new cross-file call, check whether
it's reachable from the calling file's own script-execution time (breaks) or only from a later
callback/event (safe).

**What each file requires on `window` beyond `Eden*`:** every screen file also assumes
`document`/`window` exist — none of the eleven `public/*.js` files are reachable from the headless
harnesses (there is no `document` under `node eden.js`), which is exactly why `A8`'s
`Module.__edenFramePost` hook and every headless suite in `tools/` test the *engine* side of a
feature and leave the DOM layer for a live-browser pass. See audit rows G2/B8's "needs a live
browser" notes for the concrete instances this has already blocked.

## Design notes
Every DOM surface shares one visual language — chunky beveled "arcade cabinet"
skeuomorphism, square corners, hard-edged shadows, the Jersey 10 pixel face, one lime
accent — defined in `public/eden-ui.css` and built by `public/eden-ui.js`'s factories.
**[design-system.md](design-system.md) is the reference**; don't re-derive it from the
CSS. Density is handled by the `--u` scale unit (the whole UI rescales with the
viewport) plus a `pointer: coarse` block that raises hit boxes to the 44px floor where
that scale bottoms out.

This replaced the "Dark Glass Chrome" language the settings panel used through pass 43
(dark gradient surfaces, rounded corners, a leaf-green accent, ~200 lines of CSS
injected from `eden-settings.js` as a JS string). Nothing of it remains;
`EdenSettings.injectCSS()` survives only as an alias for `EdenUI.ensureCSS()` so
existing call sites keep working.
