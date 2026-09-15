# UI Architecture

## Purpose
Every pixel of UI — HUD, menus, world browser, sharing screens, keyboard — is drawn
with OpenGL by the game itself. There is **no UIKit UI** beyond the GL view (and
`UIAlertView`-era code in `Alert.mm`). Text is rendered by rasterizing strings through
`Texture2D`'s string initializer and caching the textures.

## Building blocks
- `Texture2D` (`Classes/Texture2D.mm`) — Apple's classic texture class, extended with
  string rendering and `drawSky`/`drawInRect` helpers. UI text = one texture per
  string (regenerated when the string changes — expensive if done per frame).
- `Button` (struct in `Texture2D.h` area; used via `inbox2/inbox3` in `Util.mm`) —
  rectangle + pressed/animation state.
- `statusbar` (`Classes/statusbar.mm`) — the reusable toast/progress line
  (`setStatus(text, priority)`, `clear()`); instances owned by Hud, Menu, SharedList.
- `VKeyboard` (`Classes/VKeyboard.mm`) — custom GL keyboard for world names and
  search (the app predates reliable transparent UIKit overlays on GL).
- `Alert` (`Classes/Alert.mm`) — the original iOS `UIAlertView` modals (delete world, warp-home
  menu, pick-world-type). **Excluded on web and native**; each host supplies `showAlert*`.
- `GLWidgets` (`Classes/GLWidgets.{h,mm}`) — **added Phase N Stage 5.1.** The engine-side GL
  widget kit: `web/docs/design-system.md` transcribed into `Graphics::drawRect` fills. It owns the
  `GLW::u()` scale unit (the CSS `--u`, but against POINT space), the palette, the three bevel
  mechanics (`BEVEL_RAISED`/`SUNKEN`/`PRESSED`) plus the two surfaces (`WINDOW`/`CONTENT`), a
  word-wrapping `GLW::Label`, and `GLW::Button`. **It applies the `SCALE_*` corrections itself**, so
  the caller works entirely in point space with y up — the `sm_fill`/`gldialog_fill` idiom that used
  to be copy-pasted into every screen. Adding a widget? Add it here, not in a screen.
- `GLDialog` (`Classes/GLDialog.{h,mm}`) — **added Phase N Stage 2.5, rebuilt on `GLWidgets` in
  5.1.** An engine-drawn centred modal (display-face title, an optional wrapped body sentence, up
  to `GLDIALOG_MAX_BUTTONS` buttons in a TWO-COLUMN grid, a C-function callback) for hosts with no
  native dialog layer. **An odd final button spans both columns** — that is what puts "Cancel" on
  its own full-width row, so the order the seam passes buttons in is a layout decision. `World::update` routes input to it and freezes the screen underneath while
  `GLDialog::active()`; `World::render`'s tail draws it on top. The native seam
  (`native/src/seam/seam_link_stubs_native.mm`) points `showAlertWorldType()` (a chained
  type→height pair) and `showAlertWarpHome()` at it. **Web never activates it** — its
  `seam_link_stubs.mm` keeps the DOM overlays — so `GLDialog::active()` is permanently false there.
  `eden_ui_wants_cursor()` returns true while a `GLDialog` is up, so the native mouse-capture
  predicate releases the pointer for it — needed because `Hud::handlePickMenu` clears `inmenu` in
  the same breath as `showAlertWarpHome()`, so the "in a menu" term alone would not cover it.
- `KeybindsMenu` (`Classes/KeybindsMenu.{h,mm}`) — **added Phase N Stage 5.3.** The GL keybinds
  screen, and the kit's second consumer: a paginated list (7 rows a page, grouped
  Movement/Actions/Interface/Hotbar) of every rebindable action against its current key, drawn
  entirely in `GLWidgets` calls. **It is a CHILD of `SettingsMenu`, not a sibling** — its "Keys"
  button shows it and `SettingsMenu::update/render` delegate to it wholesale while it is up. That
  is deliberate and load-bearing: web `--wrap`s `SettingsMenu::update/render` to no-ops, so a
  screen reachable only from inside them cannot draw or take input on web, where the DOM Keys tab
  is authoritative. `SettingsMenu::showKeybinds()` is the programmatic entry point (the `--shot`
  harness and `--keybind-selftest` use it rather than hit-testing the button).
  - **The data is not here.** Bindings live in the shared keybind model — see the KEYBINDS section
    below — and this screen only reads `eden_keybind_*`.
  - **Rebind capture is armed in the model, not in the screen** (`eden_keybind_capture_begin`).
    The engine's `Input` carries touches and nothing else, so the key that closes a capture arrives
    on a path no GL screen can see: on native it comes through SDL and
    `native/src/seam/Input_native.cpp` feeds it to `eden_keybind_capture_feed()` **ahead of every
    other dispatch**, which is what stops the key you are binding also walking the player. Escape
    cancels (and therefore cannot be bound from this screen — the trade is deliberate: it is the
    one key every keyboard has, and a capture with no exit strands the player in a modal).
  - Only the PRIMARY binding is rebindable. A fixed secondary (arrow keys, the right-hand
    modifiers) is shown as dim "also X" text beside the button rather than on it — visible, because
    it is live input, but plainly not the control.
- `Joystick` (`Classes/Joystick.mm`) — virtual analog stick, owned by Hud.
- `Graphics::beginHud/endHud`, `prepareMenu/endMenu` — orthographic projection setup.
  All layout code branches on `IS_IPAD`/`IS_WIDESCREEN` with hard-coded coordinates
  in a 480×320 / 568×320 / 1024×768 space.

> **MODIFIED FROM STOCK (2026-07-31) — the point space is no longer a device constant.**
> `SCREEN_WIDTH`/`SCREEN_HEIGHT` are still the layout coordinate system every rect below is written
> in, and every number in this file is still correct *at* 568×320. What changed is that 568×320 is
> now one value among many: the web port derives the point space from the real window aspect and a
> UI-scale setting (`web/src/seam/DisplayProfile_web.mm`, audit rows D1/D4), so the same rect
> arithmetic runs at, say, 1024×640 on a desktop window. Three engine-side consequences:
>
> - **The rect math moved out of the constructors.** `Hud::layoutForScreen()` and
>   `Menu::layoutForScreen()` hold everything that reads `SCREEN_*`; the constructors now do state
>   init, build the owned objects (`statusbar`, `Joystick`) and call the layout method once. Both are
>   re-runnable and **must stay idempotent** — `Hud.mm`'s file-static margins (`marginLeft2`,
>   `marginVert`, `marginLeft`) are mutated by the layout body and are reset at the top of the
>   method so a second call does not compound them.
> - **The two hand-tuned widescreen offsets are now width-derived.** The block/colour picker's
>   `marginLeft2 += 57` and the in-game menu card's `+50 / SCREEN_WIDTH-300` were tuned for the
>   88 extra points the iPhone-5 profile had over the 480 one. They keep those exact values at 568
>   points and split any further width evenly, so the picker stays put relative to the screen centre
>   and the menu card keeps its 268×240 size and centres rather than stretching.
> - **Two vertical couplings that only 320 points held together.** Both found in live play and both
>   invisible at the stock height, which is what makes them worth reading before adding UI here:
>   - `rpaintframe` — the 402×282 card drawn *behind* the colour/block picker grid — was anchored to
>     the BOTTOM of the screen by a constant `marginVert+10`, while the grids it backs are anchored
>     to the TOP (every one of their Y values is `SCREEN_HEIGHT - margins - …`). At 320 points those
>     coincide; anywhere else the card slid out from under its own contents and rendered as an empty
>     panel with swatches floating above it. It now takes the same anchor the grids use.
>     `Hud::renderBlockScreen` used to rebuild a byte-identical local copy of that rect — it reuses
>     `rpaintframe` now, because one copy of the arithmetic is the only version that can stay right.
>   - The four mode buttons (build/mine/burn/paint) were spread PROPORTIONALLY down the right edge
>     (`k*SCREEN_HEIGHT/HUDR_NUM - HUD_BOX_SIZE - roffy`, k = 5…2). At 320 points that is a 64-point
>     pitch running from the top edge and it reads as one toolbar; in a taller point space the
>     buttons stay 45 points while the gaps grow with the screen. They now keep the 320-point pitch
>     and stay anchored to the top edge.
> - **`Menu_background`'s scrolling strips are loops now** rather than a hand-written count of two
>   ground tiles (three if widescreen) and two tree pairs. Same output at both stock widths; covers
>   whatever `SCREEN_WIDTH` is.
>
> `tools/headless-display-profile-test.js` in the web port pins the stock 568×320 rects byte-exact
> and the idempotence property. iOS behaviour is unchanged: that target is not built any more, and
> at 568×320 every number here is what it always was.

## HUD (`Classes/Hud.mm`, 2246 lines)

State machine over `mode`:
`MODE_CAMERA(0)`, `MODE_PICK_BLOCK(1)`, `MODE_BUILD(2)`, `MODE_MINE(3)`,
`MODE_BURN(4)`, `MODE_PAINT(5)`, `MODE_PICK_COLOR(6)` (`Hud.h:22-29`).

Responsibilities:
- Mode buttons (build/mine/burn/paint/camera), jump button, joystick or lefty D-pad
  (`leftymode`, `use_joystick`), in-game-menu buttons (save/home/exit/settings).
- **In-game menu** (`renderMenuScreen` / `handlePickMenu`, opened by the corner
  `ICO_OPEN_MENU` icon, gated on `inmenu`). **MODIFIED FROM STOCK** (2026-07-29): the
  `renderMenuScreen()` call in `Hud::render` is now additionally gated on
  `eden_hud_draw_menu_screen_hook` (`Hud.h`), a host hook whose default `NULL` means
  "always draw" — so on iOS this is stock behaviour exactly. The web port installs a hook
  that returns false unless the player opted into the legacy GL UI, because it draws its
  own DOM in-game menu in this panel's place; see [web/docs/ui.md](../web/docs/ui.md).
  Note that only the PANEL is suppressible — the corner open-menu icon always draws, since
  it is what opens the thing.
- **Block picker** (`renderBlockScreen` / `handlePickBlock`): `NUM_DISPLAY_BLOCKS 35`
  tiles; picking a second block for ramps (`pickSecondBlock`). Sets `blocktype`.
- **Color picker** (`renderColorPickScreen` / `handlePickColor`): the 54-color grid +
  "no color"; sets `block_paintcolor`. Also generates `colorTable[256]`
  (`genColorTable`, `Hud.mm:151`) — a **global** consumed by the mesher; the HUD owns
  the game's color palette, an inversion worth knowing about.
- `MODE_CAMERA`: hides UI, screenshot capture (`take_screenshot` →
  glReadPixels → PNG in Documents + MD5 hash → `FileManager::setImageHash`).
- Golden-cube counter, health/damage flash (`flash`, `flashcolor`), underwater tint
  (`underLiquid`), FPS counter, fade-in on load (`fade_out`, `justLoaded`).
- `worldLoaded()` resets per-world UI state.

`Hud::update` consumes touches (marks `inuse`) **before** `Player::processInput` sees
them — the ordering in `World::update` is the arbitration.

## Menu system (`Classes/Menu.mm` + satellites)

`Menu` is active in `GAME_MODE_MENU`; `activate()/deactivate()` load/free its textures
(the retina-scale juggling in `World::exitToMenu` wraps `activate`).

- **World carousel**: `loadWorlds()` lists the Documents directory, reads each file's
  header for the display name (`FileManager::getName`; files returning `error~` are
  skipped), builds a doubly-linked `WorldNode` list with preview textures
  (`<name>.png`), arrows to page, tap to load. Create (with `VKeyboard` name entry;
  flat-vs-default via `a_genFlat`), delete (confirm via `Alert`), rename.
  **An empty Documents folder means an empty list** — `selected_world` is `NULL` and every
  deref reachable from that state is guarded (`refreshfn`, `activate`, the layout block).
  Stock behaviour was to synthesise one placeholder `WorldNode` so the carousel was never
  empty; that was removed 2026-08-06 because a placeholder is indistinguishable from a saved
  world but has no file behind it, so tapping it lands in `loadWorld()`'s create-a-world branch
  and parks on the Flat/Normal question — which the player reads as "loading forever".
- **`Menu_background`** — the animated menu backdrop.
- **`SettingsMenu`** — the GL settings screen. `load()`/`save()`/`getNewWorldName()` and the
  5-slot `properties[]` array still own the engine-backed toggles (Health/Autojump/Creatures/
  Music/Sound) and their `NSUserDefaults` round-trip. **`render()`/`update()` were rewritten in
  Phase N Stage 2.5** into a generic paginated list over the shared settings table
  (`web/src/seam/Settings_web.mm`'s `kSettings[]`, reached via `eden_settings_*` scalar
  accessors) — toggles, `[-] value [+]` range steppers and `[<] label [>]` enum cyclers, labels
  rasterised with `stb_truetype`. **On web this code does not run**: `Settings_web.mm` `--wrap`s
  `SettingsMenu::update/render` to no-ops (the DOM panel owns settings there). Rows whose effect
  is web-only (`render_scale`, `dpr_cap`, `ui_scale`, `display_mode`, `display_layout`,
  `input_mode`, `legacy_menu`) are hidden via `eden_settings_native_hidden()`.
  **Stage 5.3 added a "Keys" button** to its bottom row (beside Save/prev/next), which shows
  `KeybindsMenu` and hands it the whole frame until it closes.
- **`ShareMenu`** — upload flow for the selected world.
- **`SharedList`** (`Classes/SharedList.mm`, 881 lines) — the online world browser:
  paged list (name/downloads/date columns as cached textures), sort tabs
  (newest/popular), search (VKeyboard), preview download + display, download world,
  report-world flag button. Talks to `ShareUtil`
  ([networking.md](networking.md)); `finished_dl/finished_preview_dl/
  finished_list_dl` flags are polled by `Menu::update` to integrate async results.
- `statusbar` instances show progress ("Loading World… 47%", "Converting World…").

## Dead/auxiliary UI code
- `Classes/Toolbar.mm` — compiled but apparently unreferenced by World/Hud/Menu
  (likely a pre-2.0 toolbar). Confidence: medium — grep found no live call sites.
- `Classes/Gamepad.mm` — physical controller support scaffolding, referenced by Hud
  includes; extent of use unverified.
- `settings.xib`, `prototypeViewController.xib` — legacy nibs, not part of the GL UI.

## Common pitfalls
- Layout constants are absolute pixels for specific devices; test all three of
  iPhone / widescreen iPhone / iPad ("iPad" = also Retina iPhones, see the `IS_IPAD`
  pitfall in [conventions-and-pitfalls.md](conventions-and-pitfalls.md)).
- String textures leak easily; the code caches them in structs (`SharedListNode`) and
  frees on list rebuild — follow that pattern.
- The HUD directly mutates gameplay globals (`blocktype`, `goldencubes`,
  `block_paintcolor`) that Terrain reads mid-edit.
- Menu and game share the GL context and `Resources` texture slots; menu textures are
  unloaded during play (memory), so any menu code reachable in-game must not touch
  them.

## Safe vs. risky to modify
- **Safe:** layouts, adding buttons/modes following existing patterns, statusbar
  messages.
- **Caution:** touch-consumption ordering vs. Player, `genColorTable` (changes every
  painted block in every saved world!), texture load/unload pairing across
  menu↔game transitions.


## The keybind model (Phase N Stage 5.2)

**Where it lives:** `web/src/seam/Settings_web.mm`, in its own `KEYBINDS` section — a *shared* seam
file, compiled into both the web and the native trees, so the browser's Keys tab and the engine's
`KeybindsMenu` are two front-ends over one table. Read that section's header comment before
changing anything here; the summary:

- **A binding is an integer: the key's USB HID usage ID.** SDL3's `SDL_Scancode` values *are* those
  IDs, and the browser's `event.code` strings are a 1:1 renaming of the same table. That is what
  retired the old "keybinds must live in JS because the C settings model stores floats only"
  exception — an int below 512 is exactly representable in a float and persists as an `NSNumber`.
  `native/src/seam/Input_native.cpp` carries `static_assert`s on the scancode/HID identity, so an
  SDL that renumbered would break the build rather than silently rebind every keyboard.
- **`kKeyNames[]`** is the one place the browser spelling, the HID number and the display label are
  related. `eden_keybind_code_table()` emits it as JSON for the page (nothing ever passes a string
  *into* wasm — that would mean exporting `_malloc`/`_free`).
- **`kKeybinds[]`** is the action table: id, label, group, default primary, a fixed secondary, the
  continuous/momentary dispatch class, and which hosts implement it (`fullscreen` and `settings`
  are web-only, and `eden_keybind_native_hidden()` is how the GL screen skips them — the same
  policy `eden_settings_native_hidden()` applies to settings rows).
- **These are NOT `kSettings[]` rows.** The stage plan called for a `KIND_KEY` row type inside
  `Setting`; a keybind carries four fields a float row has no place for, and `min/max/step/def`
  would all be dead weight. `KIND_KEY` exists as a kind constant for row dispatch, not as a member
  of `kSettings[]`. `eden_settings_reset_all()` resets both tables.
- **Conflicts are allowed, deliberately.** Ctrl ships bound to *both* `flyDown` and `crouch`, so a
  "clear the other binding" rule would fight the shipped configuration on the first rebind. The
  lookup is therefore an iterator (`eden_keybind_action_after(prev, code)`), and every caller must
  walk it rather than stop at the first hit.
- **Gate:** `./build/eden_native --keybind-selftest` asserts both directions of a rebind — that the
  new key works *and* that the old one has stopped — because a test of only the new key would pass
  against an input path that still had its hard-coded `switch (sc)` and merely also consulted the
  model. It measures "stopped" against an unbound control key rather than a fixed epsilon, since a
  flying player drifts and the drift is a property of the world, not of the binding.
