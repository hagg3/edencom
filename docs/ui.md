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
  (`setStatus(text, seconds)`, `clear()`; `seconds >= 1000` never expires, so the engine's `999`
  messages do, after ~16 min); instances owned by Hud, Menu, SharedList.
  **Modified from stock (N.5.11, 2026-10-10): Hud's bar calls `useToast()` and draws as
  `GLW::Toast`** (GLWidgets.h, `.eden-toast` in eden-ui.css): a dark 85% box with the 2u grey
  keyline, white Jersey 10 text, shrink-wrapped to the text and centred `du(16)` above the bottom
  edge, 0.2 s fade in/out. Every `hud->sb->setStatus()` ("World Saved", "Loading World 47%") is
  therefore a toast, and that is the call a new in-game message makes. The stock line was a
  screen-wide body-face raster with a 1 px shadow — hard to read over terrain, and it printed
  twice at opposite ends of the screen. `pos` and the alignment argument are ignored in toast
  mode; the Menu/SharedList/ShareMenu bars are stock. The pill's width comes from the raster
  seam's `eden_text_raster_measure()` (native: the stb_truetype advance loop the rasteriser
  itself uses; web: canvas `measureText`; 0 headless, where `GLW::Label::width()` falls back to
  the `kAvgGlyph` estimate). `--shot`'s `hud-status` is the artefact; `--ui-selftest` covers the
  lifetime rules.
- `VKeyboard` (`Classes/VKeyboard.mm`) — custom GL keyboard for world names and
  search (the app predates reliable transparent UIKit overlays on GL).
- `Alert` (`Classes/Alert.mm`) — the original iOS `UIAlertView` modals (delete world, warp-home
  menu, pick-world-type). **Excluded on web and native**; each host supplies `showAlert*`.
- `GLWidgets` (`Classes/GLWidgets.{h,mm}`) — **added Phase N Stage 5.1.** The engine-side GL
  widget kit: `web/docs/design-system.md` transcribed into `Graphics::drawRect` fills. It owns the
  `GLW::u()` scale unit (the CSS `--u`, but against POINT space), the palette, the three bevel
  mechanics (`BEVEL_RAISED`/`SUNKEN`/`PRESSED`) plus the two surfaces (`WINDOW`/`CONTENT`), a
  word-wrapping `GLW::Label`, `GLW::Button`, and since Stage 5.4 / N.4.5 (2026-10-04) the value
  controls `GLW::Toggle` (split pill), `GLW::Slider` (SUNKEN track + RAISED thumb, a
  beginDrag/dragTo/endDrag protocol that reports only snapped changes), `GLW::Stepper` (`[-] value
  [+]`, which is also the kit's cycler) and `GLW::TextField` (below). **Stage 5.6 (2026-10-06)
  added** `GLW::ListRow` (a selectable list entry: hairline separator, the CSS selection TINT plus a
  4u lime bar), `GLW::ScrollView` (a list's scroll model + `.eden-scrollbar`; **row-snapped**,
  because the kit has no clipping — no scissor through the translator's letterboxed, render-scaled
  FBO path — so a half-visible row would draw outside its box), `Button::setTone()` (positive /
  danger faces) and `Button::setEnabled()` (solid chrome, grey label, never hits). Wheel notches
  reach a GL list through `eden_ui_take_wheel()` (native counts them only while the mouse is not
  captured — captured, the wheel is the hotbar's; web returns 0). **Stage 5.9 (2026-10-07) added**
`GLW::TabRail` (a horizontal row of equal RAISED tiles, the selected one PRESSED — the design
system's "pressed == selected"; the web's `.eden-tabrail` is the settings screen's vertical icon
rail, this is its few-words sibling) and `GLW::progressBar()` (`.eden-progress`: SUNKEN track, lime
12% fill; a negative fraction draws an indeterminate block). **Two faces (2026-10-05):** `GLW::Label` takes a `GLW::Face` —
  `FACE_DISPLAY` (the default: Jersey 10, the design system's pixel face, for every piece of
  chrome) or `FACE_BODY` (Rubik, for descriptive sentences — `GLDialog`'s body). The
  face reaches the raster through a sticky seam switch, `eden_text_raster_set_face()`, which
  `Label` sets around its own rasters and puts back to body, so `statusbar`/`SharedList` text is
  unchanged. Native loads `Jersey10-Regular.ttf` from the bundle (linked there by
  `native/CMakeLists.txt` from `web/public/assets/fonts/`) and falls back to the body face if it
  is missing; web selects `"Jersey 10"` in the canvas raster. **Density (2026-10-05):**
  `GLW::touchFloor()` is the 44pt floor on the touch profile and 0 otherwise, and the floor
  raises HIT BOXES, not art — `Button`/`Toggle`/`Slider`/`Stepper` take `setHitRect()` — so a
  desktop screen lays rows out at ~30u and a touch one at 44pt. **It applies the `SCALE_*` corrections itself**, so
  the caller works entirely in point space with y up — the `sm_fill`/`gldialog_fill` idiom that used
  to be copy-pasted into every screen. Adding a widget? Add it here, not in a screen.
  - **Text is rasterised at the drawable's real density (N.4.8, 2026-10-04; modified from stock).**
    The 2D ortho is `SCREEN_* x SCALE_*` units and is *not* the drawable: an iPad Air 2's touch
    profile puts 1136x852 units across a 2046x1536 px box. Stock text rasters are one texel per
    unit, so every label was magnified 1.8x through `GL_MAG_FILTER NEAREST` (doubled, uneven pixels;
    the "dialogs are slightly blurry" report). `GLW::Label`, `SettingsMenu`'s `sm_text` and `statusbar` (the IS_IPAD branch) now build
    through `Texture2D(string, dims, align, font, density)` with `eden_ui_raster_density()`
    (`web/src/seam/DisplayProfile_web.mm`: drawable height / ortho height, clamped 1–4, **1 headless**
    so no hash moves). The texture remembers its `_density`; `drawText` and `drawAtPoint` divide the quad by it
    (`drawText` also snaps the origin to a device pixel), so the layout in ortho units is unchanged. Any other
    `new Texture2D(NSString*, …)` site (SharedList, stubbed on native) still rasterises at 1 texel/unit and
    can opt in the same way.
  - **A text box wider than the 1024-texel cap renders at a density below 1 (2026-10-06, Stage 5.6;
    modified from stock behaviour on the port).** `Texture2D_web.mm` clamped the TEXTURE to the cap
    but kept the box as its content size, so `_maxS` came out above 1 and `GL_REPEAT` drew the text
    two-and-a-bit times side by side — the "status line printed twice" on the main menu and on the
    HUD (`statusbar` boxes are screen-wide: 2276 ortho units on a 1138-point window). Such a box now
    rasterises at `cap / width` texels per unit and draws at its full width. Proven with the fix
    reverted (`--shot`'s `-hud-status` capture shows the second copy) and restored. **Bitmap art (HUD icons, menu atlases) is not covered**; it is
    @2x art and stays magnified, so making it crisper means a filter choice or new art.
- `GLDialog` (`Classes/GLDialog.{h,mm}`) — **added Phase N Stage 2.5, rebuilt on `GLWidgets` in
  5.1.** An engine-drawn centred modal (display-face title, an optional wrapped body sentence, up
  to `GLDIALOG_MAX_BUTTONS` buttons in a TWO-COLUMN grid, a C-function callback) for hosts with no
  native dialog layer. **An odd final button spans both columns** — that is what puts "Cancel" on
  its own full-width row, so the order the seam passes buttons in is a layout decision. `World::update` routes input to it and freezes the screen underneath while
  `GLDialog::active()`; `World::render`'s tail draws it on top. The native seam
  (`native/src/seam/seam_link_stubs_native.mm`) points `showAlertWorldType()` (a chained
  type→height pair), `showAlertWarpHome()` and, since 5.6, `showAlertDeleteConfirm()` at it.
  `GLDialog::buttonRect(j, &r)` (5.6) tells a harness where button j is. **Web never activates it** — its
  `seam_link_stubs.mm` keeps the DOM overlays — so `GLDialog::active()` is permanently false there.
  `eden_ui_wants_cursor()` returns true while a `GLDialog` is up, so the native mouse-capture
  predicate releases the pointer for it — needed because `Hud::handlePickMenu` clears `inmenu` in
  the same breath as `showAlertWarpHome()`, so the "in a menu" term alone would not cover it.
  **`GLDialog::prompt()` (N.4.5)** is the same dialog with a `GLW::TextField` above the buttons,
  focused on show: Return picks button 0, Escape the LAST one, and the callback gets the text.
  Callers check `GLDialog::textEntryAvailable()` first (false on web). `GLDialog::dismiss()` closes
  without a callback — the harness's way out of a modal.
- `KeybindsMenu` (`Classes/KeybindsMenu.{h,mm}`) — **added Phase N Stage 5.3.** The GL keybinds
  screen, and the kit's second consumer: a paginated list (grouped
  Movement/Actions/Interface/Hotbar) of every rebindable action against its current key, drawn
  entirely in `GLWidgets` calls. Since 2026-10-05 it shares `SettingsMenu`'s shape exactly —
  titlebar (Back · "Controls" · Defaults), a heading slot per group, slots per page fitted to the
  screen, a window shrink-wrapped to the column. **It is a CHILD of `SettingsMenu`, not a sibling** — its "Keys"
  button shows it and `SettingsMenu::update/render` delegate to it wholesale while it is up. That
  is deliberate and load-bearing: web `--wrap`s `SettingsMenu::update/render` to no-ops, so a
  screen reachable only from inside them cannot draw or take input on web, where the DOM Keys tab
  is authoritative. `SettingsMenu::showKeybinds()` is the programmatic entry point (the `--shot`
  harness and `--keybind-selftest` use it rather than hit-testing the button).
  - **The data is not here.** Bindings live in the shared keybind model — see the KEYBINDS section
    below — and this screen only reads `eden_keybind_*`.
  - **`Button::setRect()` re-wraps only on a width change** (5.4). Every converted screen lays out
    per frame, and until then each button re-rasterised its label — a texture upload, plus N.4.9's
    `glFlush`, per button per frame.
- **Text entry — `GLW::TextField` + the `eden_text_input_*` seam (N.4.5, 2026-10-04).** The first
  text entry on any port target (`VKeyboard.mm` is a UIKit overlay and is seam-excluded
  everywhere). The field draws itself; the platform supplies composed UTF-8 through six C calls
  declared at the bottom of `GLWidgets.h`: native implements them in
  `native/src/seam/Input_native.cpp` over `SDL_StartTextInput` (which raises the **system keyboard
  on iOS**) and `SDL_EVENT_TEXT_INPUT`, and **while a field has focus every key belongs to it** —
  Escape cancels the edit instead of opening the pause menu, "w" does not walk. Web stubs them to
  "unavailable" (`seam_link_stubs.mm`), because the DOM owns text entry until Stage 5.10. Three
  things to know: **the caret is a glyph** (`"name|"`, rasterised with the text, because the raster
  seam has no measure call and a drawn caret would drift); editing is append/backspace only,
  UTF-8-aware and capped in **bytes**; and the platform ending input on its own (the iOS keyboard
  dismissed) is `EV_BLURRED`, distinct from Return (`EV_COMMIT`) and Escape (`EV_CANCEL`).
  `GLDialog::prompt()` is the dialog form of it. **Keyboard avoidance (T.B1, 2026-10-08):** the only
  thing iOS does about an on-screen keyboard is SDL's — it shifts the whole view up until the
  *bottom of the text-input area* meets the keyboard — so a field also takes
  `setKeepVisible(rect)`, which the sixth call (`eden_text_input_keep_visible`) folds into
  that area on iOS only. `GLDialog` passes its whole panel, which keeps its buttons above the
  keyboard; desktops ignore it so the IME candidate window stays anchored on the field.  - **Rebind capture is armed in the model, not in the screen** (`eden_keybind_capture_begin`).
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
  `ICO_OPEN_MENU` icon, gated on `inmenu`). **Stage 5.5 (2026-10-05) moved it onto `GLWidgets`:**
  `layoutPauseMenu()` places the stock `rsave`/`rhome`/`rcam`/`rexit` Button rects as a column of
  kit buttons ("Paused" titlebar, a WINDOW shrink-wrapped to the column, the engine's own action
  icons on the left of each button) and sets each `rt*` text rect equal to its button, so the
  stock touch-down claim (`inbox3`) and release (`handlePickMenu` → `inbox2`) paths are unchanged.
  Two new rects: `rresume` (closes the menu) and `rsettings` (opens the GL `SettingsMenu` as an
  in-game modal; only where `eden_gl_settings_available()` — never on web, where `SettingsMenu`
  is `--wrap`'d away and there would be no way out). The button gap is kept above 8pt because
  `inbox2`/`inbox3` add 4pt of slop and `handlePickMenu` runs every action whose rect matches.
  The panel no longer fades with `at1` (the kit snaps). **MODIFIED FROM STOCK** (2026-07-29): the
  `renderMenuScreen()` call in `Hud::render` is now additionally gated on
  `eden_hud_draw_menu_screen_hook` (`Hud.h`), a host hook whose default `NULL` means
  "always draw" — so on iOS this is stock behaviour exactly. The web port installs a hook
  that returns false unless the player opted into the legacy GL UI, because it draws its
  own DOM in-game menu in this panel's place; see [web/docs/ui.md](../web/docs/ui.md).
  Note that only the PANEL is suppressible — the corner open-menu icon always draws, since
  it is what opens the thing.
- **Block picker** (`renderBlockScreen` / `handlePickBlock`): `NUM_DISPLAY_BLOCKS 35`
  tiles; picking a second block for ramps (`pickSecondBlock`). Sets `blocktype`.
  **Two pages since D.2p (2026-10-10).** Page 1 is the stock 35 (`hudBlocks[]`); page 2
  (`hudBlocks2[]`, 18 cells in the first 18 `blockBounds[]` slots) is ids 112…127 in **id order**
  (the 2026 game shows 113 first; the user chose id order), then the tools `HUD_TOOL_SIGN` (1000)
  and `HUD_TOOL_CMD` (1001). The tools are **sentinels, not block ids** — `hud_picker_is_tool()`
  guards every table read (`picker_bt()` for `hudBlocksMap`), they draw as captioned cells, and
  `Hud::pickerToolEnabled()` keeps a tool veiled (a tap is consumed, picks nothing) until its row
  turns it on. **SIGN is on since D.3c (2026-10-10) wherever GL text entry exists**
  (`GLDialog::textEntryAvailable()` — native and iOS; web stays veiled until 5.9's web text-input
  seam); **CMD since D.4b**, under the same rule. Both cells (and the HUD's current-block button
  with a tool armed) are captioned in the **body** face without the chrome shadow: the display
  face at 12pt with its 1u white shadow smudged "SIGN" (the user, D.4b). Picking an enabled tool goes through `Hud::update`'s ordinary release
  path, so `mode` = `MODE_BUILD` and `blocktype` = the sentinel; `Player::processInput` hands a
  build tap with a tool armed to the tool (`SignTool::tap` / `CmdTool::tap`) and never lets the
  sentinel become a preview type, and `renderBlockAndBorder` draws the build background + the
  tool's caption instead of an atlas face. **A sentinel must never index a block table** — note
  that `Player::test` reads `blockinfo[hud->blocktype]`, so `CmdTool` swaps in `TYPE_STEEL` around
  it. Both tools do nothing while a `GLDialog` is up (a tap under a modal never re-opens it).
  **The CMD tool** (`CmdTool`, in `Classes/SignTool.{h,mm}`): a tap on a command block (a non-air
  block with a `CMB1` record) opens a 6-line prompt prefilled with its script (511 bytes, printable
  ASCII; OK with an empty/blank answer keeps the block and clears the script, as cmd.html says); a
  tap on anything else places one in the cell in front of the face — the build rules (air or a
  liquid below level 4, not inside the player), unpainted `TYPE_STEEL` through `buildBlock`, plus a
  record with an empty script and `flags` 0 — refused with a toast at 512 per world. No prompt on
  placing: tap it again to write the script. The skin is docs/rendering.md's.
  **Pushing a command block (D.4c, `Classes/CmdScript.{h,mm}`):** a build tap with a *block* armed
  (not a tool, not holding a creature) whose `FC_DESTROY` hit is a command block runs its script —
  `CmdScript::tap`, checked in `Player::processInput` right after the tool branch — plays the door
  sound (the 2026 game's), and builds nothing. A tap on a block whose script is still running (a
  `wait`) does nothing. SIGN armed still hangs a sign on it, CMD armed still edits it. The
  hold-to-act pulse re-presses every 200 ms while held, like a held build. The language and every
  rule Emod chose where cmd.html is silent: the header comment of `CmdScript.h`;
  docs/eden-file-format.md "Command-block scripts".
  **Multi-line prompts (D.4b):** `GLDialog::prompt(..., lines)` / `GLW::TextField::setLines(n)`
  wrap the field to its width (`Label::set`'s `keepLastLines`, which also cuts a word longer than a
  line) and keep the caret's last n lines in view; Return still commits. Signs use 3, scripts 6. **The SIGN tool** (`Classes/SignTool.{h,mm}`): the face is the
  `FC_PLACE` − `FC_DESTROY` cell delta; the bottom is refused with the toast "Can't place a sign
  under the block"; otherwise `GLDialog::prompt` (95 bytes, printable ASCII kept) — a new sign,
  or, on a face that already has one, an edit prefilled with its text, where an empty answer
  deletes it. Colour = the build paint (`block_paintcolor`, set by tapping build in paint mode)
  or 45 (charcoal); an edit recolours only when a paint is picked. Refusals from the trailer
  (full, unreadable) toast through `fm->reportTrailerRefusal`. `pickerPage` + the ◀ ▶ strip (`pickerPrev` /
  `pickerNext`, laid out by `layoutForScreen()` under the grid's last row; hit boxes grow DOWNWARD
  to `touchFloor()` so they never steal the last row's taps) are the only additions: page 1's
  `blockBounds[]` are byte-identical. The window keeps page 1's size on both pages, grown over the
  strip; page dots sit between the arrows. `Hud::pickerPageStep(delta)` (wraps) is the one entry
  point: the arrows, native `[` `]` (`Input_native.cpp`, fixed keys, not keybind rows) and the
  wheel (`eden_ui_take_wheel()` in `Hud::update`), web `[` `]` + wheel (`eden-input.js` →
  `eden_picker_page_step`, which answers 0 when the picker is closed so the key/wheel keeps its
  usual meaning). Reopening the picker lands on the current block's page. A page turn keeps a
  pending Block-TNT second pick; no page-2 cell has a BT variant, so all of page 2 is veiled during it.
- **Color picker** (`renderColorPickScreen` / `handlePickColor`): the 54-color grid +
  "no color"; sets `block_paintcolor`.
- **Both pickers draw on `GLWidgets` since Stage 5.7 (2026-10-06); the hit path is stock.**
  `blockBounds[]`/`colorBounds[]`, `rpaintframe`, `handlePick*` and `Hud::update`'s release logic
  are untouched (the display-profile test pins the classic rects). Only drawing moved: a WINDOW
  bevel shrink-wrapped to the grid replaces the `ICO_COLOR_SELECT_BACKGROUND` card (`rpaintframe`
  is still computed and now drawn by nothing); each cell is a RAISED bevel — PRESSED while held
  **and for the current choice** (`blocktype`, `paintColor-1`; stock showed only the press) — with
  the atlas face or the item icon inside it; the TNT second-block pick rings the blocks it accepts
  in lime and veils the rest grey instead of the `*_BORDER_ACTIVE` glow and a 50% fade. Ramps keep
  their triangle face. Both draw only while their mode is open (the kit snaps; `at2`/`at3` still
  count but no longer fade anything). Also generates `colorTable[256]`
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

- **World list**: `loadWorlds()` lists the Documents directory, reads each file's
  header for the display name (`FileManager::getName`; files returning `error~` are
  skipped), builds a doubly-linked `WorldNode` list. Stock drew it as a carousel of atlas blocks
  with arrows and corner icons (create, delete, share, get worlds, options); tap a block to select,
  tap again to load. Create (with `VKeyboard` name entry; flat-vs-default via `a_genFlat`), delete
  (confirm via `Alert`), rename.
  **Stage 5.6 (2026-10-06, modified from stock): the screen is `GLWidgets`.** Under the logo (art,
  kept) a WINDOW holds a titlebar (Settings · "Worlds" · New), the list as `GLW::ListRow`s in a
  row-snapped `GLW::ScrollView` (drag, scrollbar, wheel) and an action bar (Delete · Rename ·
  Play, Play in the positive tone); `sbar`'s message is drawn by the kit under the window (in-game
  chrome's light-on-dark), and `fnbar`'s job — naming the selection — is the selected row's.
  `layoutKit()` runs per frame from `SCREEN_*` and the list length: rows at `max(30u,
  touchFloor())`, at least four, at most what fits, and the window shrink-wraps them. **The state
  and the actions are stock:** `selected_world`, tap-the-selected-row-to-play, the create branch,
  `loading`'s ladder in `render()`, `showAlertDeleteConfirm` → `a_deleteConfirm`, `showsettings`
  — so web's `Menu_web.mm` accessors are unaffected. The stock rects (`rect_options`,
  `rect_create`, the arrows …) are still computed by `layoutForScreen()` and nothing reads them.
  **Share** (`kit_share`, beside Rename; **S.5, 2026-10-10**, native only — `WorldShare::offered`)
  opens `Classes/WorldShare.{h,mm}`'s GLDialog chain: **Export** / **Upload** / **Remove original**
  (only for an `.emod` with its `.eden` kept) → a format picker (own format first, Legacy64z,
  NewDawn256z, NewFormat256z) → the signs question, only when pruning would drop any → a confirm
  showing the pre-flight size and loss sentence → a time-sliced job on the status bar. Export
  writes `Documents/Exports/<name>.eden.gz`; Upload is networking.md § Upload. The stock
  `rect_share`/ShareMenu flow stays unused. `WorldShare::startExport/startUpload/lastResult` skip
  the dialogs for `--browser-selftest`. **Get Worlds is,
  since Stage 5.9 (2026-10-07)**, wherever the host can fetch (`Menu::browserOffered()` =
  `WorldBrowser::available()`; never on web): a titlebar button beside New (the "Worlds" title
  yields when the two would collide) opening `WorldBrowser` — below. `controlRect(name)` (incl.
  `"getworlds"`)/`rowRect(i)` exist for the harness.
  **Native delete actually deletes since 5.6**: its `showAlertDeleteConfirm()` was a no-op ("not
  confirming is the safe default"), so the stock button did nothing at all; it is now a
  `GLDialog` (Delete · Cancel). Web's stays a no-op (the DOM menu confirms and calls
  `eden_menu_delete_at`).
  **Rename on the port (N.4.5, modified from stock):** a kit `Rename` button in the action bar
  (`Menu::renameOffered()` — hidden on web, in delete/share mode and while loading) opens
  `GLDialog::prompt`; its commit is `Menu::renameSelected()`, which trims, rejects an empty name and
  calls `FileManager::renameWorld()` — an in-place rewrite of `WorldFileHeader::name` (50 bytes,
  nothing else moves; a never-played world has no file and needs no write).
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
  accessors). **Stage 5.4 (2026-10-04) reskinned it onto `GLWidgets`**: a WINDOW panel with a
  titlebar (Back · "Settings" · Controls), CONTENT strips in a centred column, and per row a
  `GLW::Toggle`, a draggable `GLW::Slider` with a readout (KIND_RANGE; percentages for 0–1 ranges)
  or a `GLW::Stepper` with `<` `>` (KIND_ENUM). A group heading takes a slot of its own, every page
  starts with one (a continued group repeats it) and a heading never ends a page. A slider writes
  through `eden_settings_set()` on every snapped step of a drag, and only the touch that grabbed the
  thumb drives it. Back saves and closes through `eden_settings_menu_close()` (save, re-apply the
  port settings `load()` stomps, clear the touch table). **Density pass (2026-10-05, the user's
  look at 5.4):** `fit()` sets the row pitch to `max(30u, touchFloor())` and derives rows per page
  from the screen height (3 pages on a 908x512-point desktop window instead of 5); the window is a
  column as tall as the longest page, centred — it sits behind the content instead of filling the
  screen; compact controls (a ~2.4:1 toggle pill, a slider whose thumb scales with its box) whose
  touch hit boxes are the whole row slot. **Stage 5.5 also opens it in-game** (the pause menu's
  Settings button; `World::update/render` run it as a modal over the frozen world, like
  `GLDialog`), and `resetView()` makes every opening start on page 1 of Settings proper.
  Rows stay hidden and inert until
  `eden_settings_loaded()`. `pageCount()/showPage()/showRow(key, &rect)` exist for `--shot` and
  `--ui-selftest`. **On web this code does not run**: `Settings_web.mm` `--wrap`s
  `SettingsMenu::update/render` to no-ops (the DOM panel owns settings there). Rows whose effect
  is web-only (`dpr_cap`, `ui_scale`, `display_mode`, `display_layout`, `input_mode`,
  `legacy_menu`) are hidden via `eden_settings_native_hidden()`. `render_scale` was on that list
  until N.4.11 (2026-10-04); native now renders the 3D pass at it (see rendering.md, "Scene-pass
  bracket"), so the Video page shows it. **Not on iOS since 2026-10-09 (N.4.12):** the row is hidden and
  `eden_get_render_scale_pct()` returns 100 there.
  **Stage 5.3 added the keybinds entry** (the titlebar's "Controls" button since 5.4), which shows
  `KeybindsMenu` and hands it the whole frame until it closes.
- **`WorldBrowser`** (`Classes/WorldBrowser.{h,mm}`, **added Stage 5.9, 2026-10-07**) — "Get
  Worlds" on the kit, replacing `SharedList` on the port targets (where SharedList/ShareUtil are
  seam-excluded NSURLConnection code). One `TabRail` tab per SOURCE: **Archive** (the community's
  `hagg3.github.io/edenarchive` manifest — the only source the web DOM browser can reach), **Current
  server** (`app2`/`files2.edengame.net`) and **Legacy server** (`app`/`files.edengame.net`). The
  servers keep stock's modes as a second rail — Featured (`popularlist.txt`) / Recent
  (`list2.php?start=N&sort=2`, paged by **More**) — plus server-side Search on Return; the archive
  filters its whole catalogue as you type (name, author, tags). A row tap selects (a tap on the
  selection downloads, as the menu plays); the detail pane shows the preview (`<id>.eden.png`,
  fetched 0.25 s after the selection settles), the date (an official id IS the upload's unix time),
  author / size / tags where the archive has them, and the download's progress. **Download** streams
  through the net seam to `<saves>/.emod-download(.part)`, then **unpacks a few MB per frame** over
  zlib — gzip (multi-member), a single-entry zip (central directory trusted over a streamed local
  header) and the archive's zip-in-a-zip, up to four layers — and imports: the result must describe
  itself (`WorldFileHeader::directory_offset` inside the file), lands as `<id>.eden` or `<id>-N.eden`
  (never replacing a world the player already has), is named from its own header, added as a
  `WorldNode` and selected, and the browser hands back to the menu. Ids that are not
  `[A-Za-z0-9_-]` drop their entry (they become paths); names are UTF-8-cleaned before they become
  textures. Back cancels an in-flight download. Each source keeps its list, scroll, selection and
  query across tab switches and across closing the screen. **The HTTP stack is the host's** —
  `eden_net_*` (declared in `WorldBrowser.h`): native `src/seam/Net_native.cpp` (NSURLSession /
  libcurl / WinHTTP, `native/README.md`), web stubs reporting no network (a page cannot reach the
  plain-HTTP, CORS-less Eden servers; the DOM screen stays the web's browser until 5.10).
  Harness: `--browser-selftest` (offline fixtures), `--net-live-selftest`, `--shot`'s `-browser*`.
- **`ShareMenu`** — upload flow for the selected world.
- **`SharedList`** (`Classes/SharedList.mm`, 881 lines) — the STOCK online world browser
  (seam-excluded on every port target; `WorldBrowser` above is the port's):
  paged list (name/downloads/date columns as cached textures), sort tabs
  (newest/popular), search (VKeyboard), preview download + display, download world,
  report-world flag button. Talks to `ShareUtil`
  ([networking.md](networking.md)); `finished_dl/finished_preview_dl/
  finished_list_dl` flags are polled by `Menu::update` to integrate async results.
- `statusbar` instances show progress ("Loading World… 47%", "Converting World…").
  `statusbar::current()` (5.6) returns the live message for a screen that draws it in the kit.

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
