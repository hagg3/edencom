# UI Architecture (the port layer)

> First written for the web build (retired 2026-10-13); its DOM screens were cut in W.5 and are at tag
> `web-port-final` ([web-port-archive](../../docs/web-port-archive.md)). Where this page says "CSS
> pixels", read SDL window points on native; the arithmetic is the same.

Read [`../../docs/ui.md`](../../docs/ui.md) first — the custom-GL HUD/menu system (`Hud.cpp`/`Menu.cpp`) is authoritative for state and side effects; this page covers the port's settings tables, the derived point space and the seam accessors. Visual language: [design-system.md](design-system.md).

## Settings (`src/seam/Settings.cpp`)
`SettingsMenu`'s data model (`properties[]`, `load()`, `save()`, `getNewWorldName`) stays authoritative; the GL screen (`Classes/SettingsMenu`) renders the C tables. (The web build `--wrap`ped its `update()`/`render()` to no-ops under a DOM panel — retired.)

One C table, `kSettings[]` (`src/seam/Settings.cpp`), is the single source of
truth for what the settings screen shows — `eden_settings_count()`/`_label()`… expose it and
`Classes/SettingsMenu` renders it generically. **Add a setting in C.** About 40 rows (a few keys appear
twice under `#if` for per-platform defaults) in the groups `Gameplay`, `Audio`, `Controls`, `Video`,
`Interface` (shown on the Video tab), `Experiments` (opt-in, non-stock: fly, `fps_normalize`,
`crouch` — see [player-input-camera.md](player-input-camera.md) — fire rules,
creature walking sounds, the double-size brush, `camera_view` (H.12)) and `Saves` (`save_backup`, `autosave`, `upgrade_256z`). The GL screen's seven
tabs and the hidden web-only rows are in [../../docs/ui.md](../../docs/ui.md) (`SettingsMenu`). Rows that
are visible but inert on native: `crosshair` and `save_backup` since S.8
(docs/save-load.md).

**Trap**: `SettingsMenu::load()` hard-codes `player->invertcam = FALSE` and
`hud->use_joystick = TRUE` on **every** load (`Classes/SettingsMenu.cpp`) — this is
engine behavior, not a bug to fix. The port's own `eden_apply_port_settings()` must
re-run after every `save()`/`load()` or these two values silently revert.

Keybinds live in a second C table, `kKeybinds[]`, parallel to `kSettings[]` in the same file (a binding is a USB HID usage ID — the number SDL3 scancodes use, which fits in a float; see the file's header for why a *parallel* table). Persistence, defaults and the code↔action map are all in C (`eden_keybind_*`, `eden_keybind_code_table()`); `Classes/KeybindsMenu` is the rebind UI. (Through Stage 5.1 a JS `localStorage` blob owned them — retired.)
See [conventions-and-pitfalls.md](conventions-and-pitfalls.md) #6.

`eden_settings_reset_all()` loops `kSettings[]` and also calls `eden_keybind_reset_all()` — one reset for both tables.

Since G.2b-1 every target keeps preferences in one flat file through `eden_prefs` (`port/src/shim/foundation/eden_prefs.h`, store `port/src/seam/Prefs_native.cpp`); before it, `NSUserDefaults` was Apple's on macOS/iOS and that file-backed store on Linux/Windows (the web version wrote `localStorage`). Mac/iPad settings reset once on the switch (accepted, de-ObjC plan D7).

## The menu key over the main menu (`src/seam/Menu.cpp`)
`Input_native.cpp` sends the `menu` keybind (Escape) down a chain of `eden_*_back()` calls, each 1 when it
consumed the key: Console → Settings → New World → World Properties → **`eden_menu_home_back()`** (I.7:
the world list returns to the home screen; 0 on home, mid-load, under a `GLDialog`, or with any screen
over the list) → otherwise the HUD's menu tap. A new screen over the main menu adds its `_back` before
the home one. MP.6: **`eden_server_list_back()`** sits just before it (form → cancel a join → home).

Text-input kinds (MP.6): `eden_text_input_start_kind(kind, …)` maps `EDEN_TEXT_KIND_CODE`/`USERNAME`/`PASSWORD`
to SDL3's text-input types (password = `SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN`, so the OS keyboard hides it and
skips autocorrect); `GLW::TextField::setKind()` picks it, `setSecure()` draws one `*` per code point
(`GLDialog::setPromptSecure` for prompts).

## Retired DOM surfaces
The web build's DOM screens — pause menu, loading overlay, load-failure dialog, alert/dialog seams, Storage tab,
the JS/wasm boundary conventions, the Load/New World menu, the `public/*.js` dependency graph and its
"Dark Glass"/arcade-cabinet CSS — were cut from this page on 2026-10-13 (W.5). They are history at tag
`web-port-final` (`git show web-port-final:web/docs/ui.md`). Their native replacements are the GL screens
(`Classes/Menu`, `GLDialog`, `NewWorldMenu`, `SettingsMenu`, `KeybindsMenu`, `WorldBrowser`; see
`../../docs/ui.md`) and the visual language in [design-system.md](design-system.md).

## The engine's retina/quality swap is ignored on purpose (audit row 22 / B7)
`World::update()` returns a bool meaning "swap graphics quality"; on iOS
`EdenViewController.mm` answered it by flipping `IS_IPAD`/`IS_RETINA`/`SCALE_WIDTH`/
`SCALE_HEIGHT` and recreating the framebuffer at the new density. **This port
deliberately does neither**, and `EdenViewController::drawFrame()`
(`port/src/seam/EdenViewController_native.cpp`) carries the reasoning. The short version, because it is the thing to understand before touching any
UI layout here:

> **`IS_IPAD`/`IS_RETINA`/`SCALE_*` are this port's layout coordinate system, not its
> resolution.** The engine lays every HUD and menu element out in a POINT space at 2×; the real
> drawable is decoupled entirely (the window's pixel size via `eden_set_drawable_size`, with the 3D
> pass optionally at `render_scale`). Flipping those globals does not make pixels cheaper — it
> halves the UI's own layout math underneath an unchanged surface.

The port already exposes the *intent* ("cheaper pixels") as a real setting, `render_scale`
(and, on the web, `dpr_cap`). The branch used to flip the globals without recreating
anything, which was the worst of both worlds; it is now an explicit no-op that announces
itself once under `EDEN_DIAGNOSTICS` if it ever becomes reachable. It is unreachable
today — `World::update` only requests a swap when `!bestGraphics`, and nothing in this
port ever sets `LOW_MEM_DEVICE`/`LOW_GRAPHICS`, while `IS_WIDESCREEN` (always true for any point
space this port can derive) forces `bestGraphics` back on regardless.

## The point space: derived, not pinned (audit rows D1 + D4)
Until 2026-07-31 that point space was pinned at 568×320 (the web shell's `establishScreenMetrics`), which is what made the HUD *enormous* on a desktop monitor: a 45-point
button drawn into a 2560-wide window came out 4.5× its design size. The point space is now derived,
and the whole of that derivation — plus the two-profile concept it hangs off — lives in
[`src/seam/DisplayProfile.cpp`](../src/seam/DisplayProfile.cpp). **Read that file's header
before changing anything about layout, sizing or the settings rows below.** The summary:

```
SCREEN_HEIGHT = 640 * 100 / ui_scale_pct        # a user/profile choice: UI DENSITY
SCREEN_WIDTH  = round_even(SCREEN_HEIGHT * aspect)   # the real window's aspect (clamped 1.2–2.4)
P_ASPECT_RATIO = SCREEN_WIDTH / SCREEN_HEIGHT        # ALWAYS derived — see below
```

- **The safe area (I.2).** The host reports the window's unsafe insets in window points
  (`eden_display_set_safe_insets(l,t,r,b)`; native: `SDL_GetWindowSafeArea` in
  `DisplayMode_native.cpp` after each point-space derivation and on
  `SDL_EVENT_WINDOW_SAFE_AREA_CHANGED`, or `--safe-area=`/`--device=`). `eden_safe_rect()` converts
  them through the letterbox (a bar already covering an edge cancels that inset) into engine points,
  y up, rounded outward, live (never cached); a change re-runs the Hud/Menu layout. The engine reads
  it as `GLW::safeRect()` (`docs/ui.md`). `eden_display_debug_ignore_safe_area()` is the selftest's
  "would it fail without insets" switch; `eden_debug_safe_area_violations()` (DebugState.cpp) is the check.
- **Window points per engine point (I.3).** `eden_display_window_pt_per_point()` is the letterboxed
  box's height in window points over `SCREEN_HEIGHT` (the snapped case uses px-per-point), 1 when no
  box is known (headless). `GLW::touchFloor()` divides 44 by it, so the touch floor is 44 OS points.
- **Two profiles, `desktop` and `touch`**, from the input-mode arbitration
  (`eden_effective_input_is_touch()` — one detector, one user override, for both concepts; native
  sets the detector once at boot, [player-input-camera.md](player-input-camera.md)). A
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
- **On the desktop profile the point space snaps to whole device pixels per point (X.20,
  2026-10-17).** Measured: 125% in the default 1136×640 window gave 0.625 px per ortho unit on a 1x
  monitor and 1.25 on a 2x one, and every engine image is @2x art (1 texel per ortho unit) sampled
  `GL_NEAREST`, so texels were dropped or doubled unevenly and text (then rasterised at ≥ 1 texel per
  unit) was minified — the Mac "blurry UI" report. The native host now passes the box in pixels too
  (`eden_display_set_viewport_px`, `port/src/seam/DisplayMode_native.cpp`); `eden_display_snap`
  picks the whole `p` px/point whose height `box_px / p` is log-nearest the `ui_scale` target, preferring
  p = 1 or even (art exact: 2:1 down at 1, 1:1 at 2, 2× at 4) — an odd p ≥ 3 must be 25% closer; the
  host then letterboxes to exactly point space × p (bars < p px), and `eden_ui_raster_density()` is
  p/2 on the nose. The point space may be odd (960×525). Consequences: `ui_scale` is coarser (in a
  maximised 1080p window 100/125/150% all land on p = 2; 200% gets p = 3), and dragging a window's
  height across a threshold steps the UI size. Touch is never snapped (finger-tuned sizes; the iPad
  was not blurry), nor is anything that only calls `eden_display_set_viewport` (headless, selftests).
  Gate: `--display-selftest` §7. Jersey 10 (the display face) is a pixel font on a 75-unit grid;
  `face_scale` in `TextRaster_native.cpp` snaps its raster scale so a grid cell is a whole number of device
  px (X.22a; size moves ≤ ½ cell, measure and raster share it). Residual (X.23): at p = 1 the art is a
  NEAREST 2:1 reduction (the 3D world shares the non-mip texture path, so its MIN filter was left).
- **A density floor keeps small windows usable.** `ui_scale` alone gives a fixed point space, i.e. a
  UI that is a fixed *fraction* of the canvas — so shrinking the window shrinks every HUD icon with
  it. That is right on a monitor and wrong the moment the window gets phone-shaped, and nothing
  about resizing a mouse-driven window flips the touch profile, so the desktop 512-point space stays
  in force and the 45-point buttons keep shrinking past ~35 CSS px (reported from live play,
  2026-07-31). The floor is **one engine point is never smaller than one CSS pixel** — not an
  arbitrary number: on the iPhone 5 the viewport was 568×320 CSS pixels and the point space was
  568×320, so UIKit points *are* CSS pixels (and SDL window points on native) and this says "never denser than the device the art was
  drawn for". It is applied against the letterboxed canvas box, not the raw viewport, and it makes a
  phone-shaped window degrade toward the classic layout instead of a miniature of the desktop one.
- **Two settings rows expose it**, both defaulting to `Auto`: `ui_scale`
  (`Auto,100%,125%,150%,200%`) and `display_layout` (`Auto,Classic 16:9,Adaptive`). `Auto` is not
  the same as picking the profile's value by hand — it re-resolves if the input mode changes.
  Deliberately *not* seeded by the profile-defaults writer for that reason; `dpr_cap`/`fps_cap`/
  `render_scale`, which have no `Auto` option, still are.
- <a name="low-memory-overlay"></a>**A third row, `low-mem`, is an *overlay*, not an input profile
  (ROADMAP Phase M / M5.2) — web-only: native never calls `eden_set_low_memory()` (decision in
  `main()`, N.4.10).** `eden_active_profile()` never returns it — the web page called
  `eden_set_low_memory(1)` (before `eden_settings_init`) when `navigator.deviceMemory ≤ 4`, when a
  prior threaded-build load-failure downgrade is remembered, or on `?lowmem=1`. While set,
  Settings.cpp's profile-default seeder takes `dpr_cap` / `render_scale` (and `fps_cap` if the
  row sets it) from `kProfiles[EDEN_PROFILE_LOWMEM]` — 1× pixel ratio, 75% render scale, 45 fps (30 on iOS, which dropped the 45 entry in P.14: its `fps_cap` list is `Uncapped,30,60`, profile indices are remapped in the seeder and stored ones migrate once via the `fps_cap_v2` pref) —
  instead of the desktop/touch row underneath; `ui_scale` / layout / control chrome are unaffected.
  Still a *default*: a row the player has touched wins. It also makes `World::loadWorld` refuse a
  256z world (docs/execution-flow.md "World loading flow").
- **A stock bug not reproduced:** `EAGLView initWithCoder:` only recomputes `P_ASPECT_RATIO`
  inside `if(IS_WIDESCREEN)`, leaving the other branch on an iPad 4:3 default that matched no live
  layout. This always derives it. (iPad's own 4:3 branch is *not* resurrected — it is commented out
  in the original and never drove a live layout on any device. See
  [`../../WORKING/archive/aspect-ratio-toggle-scope.md`](../../WORKING/archive/aspect-ratio-toggle-scope.md).)

Engine-side, this needed `Hud::layoutForScreen()` / `Menu::layoutForScreen()` (rect math lifted out
of the constructors so it can run more than once, and kept idempotent) and
`Input::screenMetricsChanged()` — see root [ui.md](../../docs/ui.md) and
[conventions-and-pitfalls.md](../../docs/conventions-and-pitfalls.md) for those. Host-side,
`port/src/seam/DisplayMode_native.cpp` hands the window's point size (`SDL_GetWindowSize`) to
`eden_display_set_viewport()` *first*, then letterboxes the drawable to whatever aspect the engine
chose (`eden_native_gl_set_letterbox`) — the box's own aspect in Adaptive mode, so the letterbox is
normally a no-op.

**Three invariants that will bite silently if broken:**
0. **Vertically-coupled UI must share an anchor edge.** Two pairs in `Classes/Hud.cpp` were anchored
   to *opposite* edges and only agreed at 320 points — the picker card vs. its own swatch grid, and
   the mode-button column's proportional spread. Both shipped broken in the first cut of D1 and were
   caught by looking at the game, not by the suite. Root `docs/ui.md` has the detail; the lesson for
   anything added here is that "it lines up at 568×320" is now evidence of nothing.
1. **`GL_VIEWPORT` must report the point space × `SCALE_*`, never the real drawable.**
   `Util.cpp`'s `findWorldCoords` scales a point-space tap by `SCALE_*` and unprojects it against
   whatever `GL_VIEWPORT` says, so the two have to agree. The GL shim's `kPickViewport` is that
   answer (`R::pickViewport()` since G.5b-3: picking reads it and the renderer's matrix stacks, no `glGet*`)
   and `eden_gl_set_pick_viewport()` keeps it in step; answering the real drawable there is
   what once made mobile taps land left of the finger (perf-audit item #6). Wrong by a constant
   factor = invisible at the screen centre, worse toward the edges — a centre-of-screen smoke test
   cannot see it.
2. **Re-layout must be idempotent.** `Hud.cpp`'s margins are file-static and *mutated* by the layout
   body; they are reset at the top of `layoutForScreen()` for exactly this reason.

Native `--display-selftest` (W.2; it replaced the web `headless-display-profile-test.js`) pins both
invariants, the stock 568×320 rects byte-exact, the derivation, the clamps, and the profile's
control-chrome field.

**Live-verified on desktop 2026-07-31** (menu, in-world HUD, mode column, picker card, window
resize down to phone proportions) — that pass is what found the two anchor bugs and the missing
density floor, none of which the headless suite could see. The touch profile has since run on real
hardware (the native iOS build on an iPad Air 2, 2026-09-07 onward — the device pass that retired the
16:9 pin above).

## Settings rows of kind TEXT (MP.8)

`kSettings[]` rows are toggle / range / enum, plus (since MP.8) `KIND_TEXT` (= 4): the value is a string in `g_text[]`, persisted as an
NSString under the row's key (`eden_settings_text` / `eden_settings_set_text`), with no float meaning; `eden_settings_set`,
`eden_settings_reset_all` (clears it) and the Console bridge (`bridgeSettings` skips kind 4; `--console-selftest` gate 3 too) know it.
`SettingsMenu` draws it as a `GLW::Button` that opens `GLDialog::prompt`. The only row is Experiments › Matchmakers (`mp_matchmakers`).

## Console seam (C.3/C.4)

Moved from `port/CLAUDE.md` (2026-10-20). The Console (C.3) is a screen over a running world, so its seam is small and shared: `Menu.cpp`
  has `eden_console_menu_{open,toggle,back,active}` and `eden_ui_captures_keys()`; `Input_native.cpp` lets
  only the `menu` and `console` keybinds through while the Console is open (typing can never walk) and
  drops the one backquote the opening key may deliver; `Input.cpp`'s `eden_ui_wants_cursor()` is 1
  while it is open. `console` is a `KB_NATIVE` row in `Settings.cpp`. C.4: `Input_native.cpp` adds
  `eden_text_input_keyboard_top()` (iOS reads `IOSKeyboard_native.mm`, its own real-UIKit ARC object
  library like `eden_apple_net`; -1 for up to 600 ms after text input starts until the first frame
  notification), `eden_text_input_start_code()` (`SDL_StartTextInputWithProperties`, autocorrect and
  capitalisation off) and Tab/Up/Down as bytes 0x09/0x11/0x12 while a field is focused;
  `eden_native_debug_set_keyboard_top()` fakes a keyboard for `--ui-selftest`/`--shot`.
