# UI Architecture

## Purpose
Every pixel of UI — HUD, menus, world browser, sharing screens, keyboard — is drawn
with OpenGL by the game itself. There is **no UIKit UI** beyond the GL view (and
`UIAlertView`-era code in `Alert.mm`). Text is rendered by rasterizing strings through
`Texture2D`'s string initializer and caching the textures. On the port the GL UI is the
*only* UI on every target (the retired web build drew DOM screens; see
[port/docs/ui.md](../port/docs/ui.md) for that history).

## Building blocks
- `Texture2D` (`Classes/Texture2D.mm`; on the port seam-excluded for `port/src/seam/Texture2D.cpp`,
  which rasterises through the `eden_text_raster_*` seam) — Apple's classic texture class, extended with
  string rendering and `drawSky`/`drawInRect` helpers. UI text = one texture per
  string (regenerated when the string changes — expensive if done per frame).
- `Button` (struct in `Texture2D.h` area; used via `inbox2/inbox3` in `Util.cpp`) —
  rectangle + pressed/animation state.
- `statusbar` (`Classes/statusbar.cpp`) — the reusable toast/progress line
  (`setStatus(text, seconds)` with a `const std::string&` since G.2b-2, `clear()`; `seconds >= 1000` never expires, so the engine's `999`
  messages do, after ~16 min); instances owned by Hud, Menu, SharedList.
  **Modified from stock (N.5.11, 2026-10-10): Hud's bar calls `useToast()` and draws as
  `GLW::Toast`** (GLWidgets.h; the design system's `.eden-toast`): a dark 85% box with the 2u grey
  keyline, white Jersey 10 text, shrink-wrapped to the text and centred `du(16)` above the bottom
  edge, 0.2 s fade in/out. Every `hud->sb->setStatus()` ("World Saved", "Loading World 47%") is
  therefore a toast, and that is the call a new in-game message makes. The stock line was a
  screen-wide body-face raster with a 1 px shadow — hard to read over terrain, and it printed
  twice at opposite ends of the screen. `pos` and the alignment argument are ignored in toast
  mode; the Menu/SharedList/ShareMenu bars are stock. The pill's width comes from the raster
  seam's `eden_text_raster_measure()` (native: the stb_truetype advance loop the rasteriser
  itself uses; 0 headless, where `GLW::Label::width()` falls back to
  the `kAvgGlyph` estimate). `--shot`'s `hud-status` is the artefact; `--ui-selftest` covers the
  lifetime rules.
- **Warp cover (P.5, 2026-10-13).** `Hud::awarpHome` arms `delayedtimer=2`/`delayedaction=6`; while it counts down
  (two frames before the synchronous `warpToHome`) `Hud::render` draws an 80 % black overlay (the same quad as
  `fade_out`) and redraws the status bar on top, so the 220–330 ms freeze reads as "Saving and warping home..".
  Death warps already fade to black via `fade_out`.
- `VKeyboard` (`Classes/VKeyboard.mm`) — custom GL keyboard for world names and
  search (the app predates reliable transparent UIKit overlays on GL).
- `Alert` (`Classes/Alert.mm`) — the original iOS `UIAlertView` modals (delete world, warp-home
  menu, pick-world-type). **Seam-excluded on every port target**; `port/src/seam/seam_link_stubs_native.cpp`
  supplies the `showAlert*` functions over `GLDialog` (below).
- `GLWidgets` (`Classes/GLWidgets.{h,mm}`) — **added Phase N Stage 5.1.** The engine-side GL
  widget kit: `port/docs/design-system.md` transcribed into `Graphics::drawRect` fills. It owns the
  `GLW::u()` scale unit (the CSS `--u`, but against POINT space), the palette, the three bevel
  mechanics (`BEVEL_RAISED`/`SUNKEN`/`PRESSED`) plus the two surfaces (`WINDOW`/`CONTENT`), a
  word-wrapping `GLW::Label`, `GLW::Button`, and since Stage 5.4 / N.4.5 (2026-10-04) the value
  controls `GLW::Toggle` (split pill), `GLW::Slider` (SUNKEN track + RAISED thumb, a
  beginDrag/dragTo/endDrag protocol that reports only snapped changes), `GLW::Stepper` (`[-] value
  [+]`, which is also the kit's cycler) and `GLW::TextField` (below). **Stage 5.6 (2026-10-06)
  added** `GLW::ListRow` (I.5 `setDetail`: a right-aligned muted date, drawn only when it fits; a selectable list entry: hairline separator, the CSS selection TINT plus a
  4u lime bar), `GLW::ScrollView` (a list's scroll model + `.eden-scrollbar`; **row-snapped**,
  because the kit has no clipping — no scissor through the translator's letterboxed, render-scaled
  FBO path — so a half-visible row would draw outside its box), `Button::setTone()` (positive /
  danger faces) and `Button::setEnabled()` (solid chrome, grey label, never hits). Wheel notches
  reach a GL list through `eden_ui_take_wheel()` (native counts them only while the mouse is not
  captured — captured, the wheel is the hotbar's). **Stage 5.9 (2026-10-07) added**
`GLW::TabRail` (a horizontal row of equal RAISED tiles, the selected one PRESSED — the design
system's "pressed == selected"; the design system's `.eden-tabrail` is the vertical icon rail
(`GLW::IconRail`, below), this is its few-words sibling) and `GLW::progressBar()` (`.eden-progress`: SUNKEN track, lime
12% fill; a negative fraction draws an indeterminate block). **Stage U.1 (2026-10-12) added** the
pieces the settings / New World rebuild (U.3, U.5) is laid out from, all drawn on the `kit` page of
`--shot`: `GLW::icon(id, rect, colour)` (one cell of the Lucide atlas `media/ui/ui_icons.png`, ids in
generated `Classes/UIIcons.h`, white art tinted at draw time; loaded on first use **with mips** — since
P.15 a non-mip load builds no chain, and its `LINEAR_MIPMAP_LINEAR` filter then sampled an incomplete
texture: every icon a black square until U.5 caught it — LINEAR + clamped unlike the engine's NEAREST art), `GLW::IconRail` (the vertical `.eden-tabrail`: 64u RAISED icon
tiles in a 72u rail, selected/held PRESSED, disabled tiles muted and inert, no count cap — it scrolls
by whole tiles of its own when they overflow; I.6: a dark fade + chevron over the clipped end says so), the toggle restyled to the
reference (heavy 2u gray-800 keyline, gray-500 off half, `Toggle::designW()/designH()` = 104×34u) with
a **muted** state (lighter, still opaque, display-only), `Button::sizeToFit(padX)` (`.eden-btn--sm`
**FT.9 fit rule (`Button::rebuildLabel`):** a label wider than its room steps its point size down (×0.92 per step, floor 70%) to stay on one line; a fixed-width button only wraps again below the floor, and a `sizeToFit` button takes `setMaxWidth(w)` (0 = none) as its room (the MP *Playing as NAME* button). Text and width alone decide the size, so nothing flickers. Not yet done: a per-menu extent-vs-rect selftest sweep and the Settings Jersey sizing pass.
shrink-wrap from `Label::width()`, 64u floor; `fitWidth()` for right-aligning) and
`Button::setIcon()` (a 16u leading icon, or a centred tile icon with no label), **variable row
heights** in `GLW::ScrollView` (`setRowHeights()`: `visible()` is how many whole rows from `first()`
fit, a row taller than the view still shows; `rowOffset()`; content drags walk the real heights;
`setRows()` is the unchanged uniform mode), and `GLW::TwoLineRow` (32u display title over a wrapping
15u body hint, hairline, indent, muted palette; reserves a right-hand control column and reports
`controlRect()` and a `height()` taken from the built labels). **Two faces (2026-10-05):** `GLW::Label` takes a `GLW::Face` —
  `FACE_DISPLAY` (the default: Jersey 10, the design system's pixel face, for every piece of
  chrome) or `FACE_BODY` (Rubik, for descriptive sentences — `GLDialog`'s body). The
  face reaches the raster through a sticky seam switch, `eden_text_raster_set_face()`, which
  `Label` sets around its own rasters and puts back to body, so `statusbar`/`SharedList` text is
  unchanged. Native loads `Jersey10-Regular.ttf` from the bundle (linked there by
  `native/CMakeLists.txt` from `media/fonts/`) and falls back to the body face if it
  is missing. **Density (2026-10-05):**
  `GLW::touchFloor()` is the 44pt floor on the touch profile and 0 otherwise, and the floor
  raises HIT BOXES, not art — `Button`/`Toggle`/`Slider`/`Stepper` take `setHitRect()` — so a
  desktop screen lays rows out at ~30u and a touch one at 44pt. **I.3 (2026-10-17; modified):
  the 44 is OS points** (iOS pt, macOS pt), converted by the seam's
  `eden_display_window_pt_per_point()`: ~36 engine points on an iPhone 16 (1.23 iOS pt each), 38
  on an iPhone SE, 24 on an iPad Air 2 (1.80), 44 headless. It was 44 *engine* points = 54/79 iOS
  pt, the phone's "huge padding" finding. `GLW::controlH(designU)` = `max(du(designU),
  touchFloor())` is the one height token (Menu, WorldBrowser, pause menu, Console, GLDialog). `u()`'s
  0.70 floor stays: 0.55 on touch was tried from shots and dropped (hint text ~10 iOS pt, the
  `railFrame` panel narrower than the phone, labels built at the old scale clipped). Every kit
  control's `render()` feeds `GLW::debugRecordControls`/`debugControlsOutside`, `--fit-selftest`'s
  recorder; UX.2 added `debugControlsOverlap` (two leaf controls drawn over each other by >1 pt, one inside the other excused for a Stepper's arrows; modal dialogs `rename`/`sign-prompt` skipped), a second check per screen. The collapsed Console strip now anchors to `safeRect()` (the gate found it under the
  island). **It applies the `SCALE_*` corrections itself**, so
  the caller works entirely in point space with y up — the `sm_fill`/`gldialog_fill` idiom that used
  to be copy-pasted into every screen. Adding a widget? Add it here, not in a screen.
  - **Text is rasterised at the drawable's real density (N.4.8, 2026-10-04; modified from stock).**
    The 2D ortho is `SCREEN_* x SCALE_*` units and is *not* the drawable: an iPad Air 2's touch
    profile puts 1136x852 units across a 2046x1536 px box. Stock text rasters are one texel per
    unit, so every label was magnified 1.8x through `GL_MAG_FILTER NEAREST` (doubled, uneven pixels;
    the "dialogs are slightly blurry" report). `GLW::Label`, `SettingsMenu`'s `sm_text` and `statusbar` (the IS_IPAD branch) now build
    through `Texture2D(string, dims, align, font, density)` with `eden_ui_raster_density()`
    (`port/src/seam/DisplayProfile.cpp`: drawable height / ortho height, clamped 0.5–4, **1 headless**
    so no hash moves; the floor was 1 until X.20, 2026-10-17, which minified every label 0.625x through
    NEAREST in a default window on a 1x Mac monitor). Since Q.2 (2026-10-22) such a label is uploaded at its W × H raster, not padded into a POT buffer.
    The texture remembers its `_density`; `drawText` and `drawAtPoint` divide the quad by it
    (`drawText` also snaps the origin to a device pixel), so the layout in ortho units is unchanged. Any other
    `new Texture2D(NSString*, …)` site (SharedList, stubbed on native) still rasterises at 1 texel/unit and
    can opt in the same way.
  - **A text box wider than the 1024-texel cap renders at a density below 1 (2026-10-06, Stage 5.6;
    modified from stock behaviour on the port).** `Texture2D.cpp` clamped the TEXTURE to the cap
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
  (`port/src/seam/seam_link_stubs_native.cpp`) points `showAlertWorldType()` (a chained
  type→height pair; since U.5 only for a world not made through `NewWorldMenu`, which parks both answers), `showAlertWarpHome()`, since 5.6 `showAlertDeleteConfirm()`, and since S.5 `showAlertDeleteOriginal()`
  (Share › Remove original) and `showAlertConvertHeight()` (convert-on-play's Upgrade to 256 · Keep 64 · Cancel) at it.
  `GLDialog::buttonRect(j, &r)` (5.6) tells a harness where button j is. **UX.9:** Escape over any open dialog is its **last button** (`GLDialog::escape()`, reached through the seam's `eden_dialog_escape`, first in the Escape chain; a prompt's field does this itself), so put Cancel / Keep editing last.
  `eden_ui_wants_cursor()` returns true while a `GLDialog` is up, so the native mouse-capture
  predicate releases the pointer for it — needed because `Hud::handlePickMenu` clears `inmenu` in
  the same breath as `showAlertWarpHome()`, so the "in a menu" term alone would not cover it.
  **`GLDialog::prompt()` (N.4.5)** is the same dialog with a `GLW::TextField` above the buttons,
  focused on show: Return picks button 0, Escape the LAST one, and the callback gets the text.
  Callers check `GLDialog::textEntryAvailable()` first (true on every native target). `GLDialog::dismiss()` closes
  without a callback — the harness's way out of a modal.
- `KeybindsMenu` (`Classes/KeybindsMenu.{h,mm}`) — **added Phase N Stage 5.3; since U.4 (2026-10-12)
  the CONTENT of `SettingsMenu`'s Keys tab, not a screen.** Every rebindable action against its current key,
  grouped Movement/Actions/Interface/Hotbar: a small grey heading per group, then a `GLW::TwoLineRow` per action
  (title · "also X" as the second line · a 96×29u key button; on touch the button's hit box is the row's height),
  in its own variable-height `GLW::ScrollView`. `SettingsMenu` owns the panel, the titlebar (Back · "Keys
  Settings" · **Defaults**), the rail, the touch claim, the wheel and the drag/scrollbar gestures, and forwards
  row taps (`down()`/`up()`: pressed on the down edge, a capture armed on release, so a drag that starts on a
  button still scrolls). While a capture is armed the whole screen is inert and any tap cancels it; the
  "Press a key" modal (`renderCapture`) is a second full-screen scrim. `SettingsMenu::showKeybinds()` selects the tab; `keyRect(action, &r)` /
  `defaultsRect()` are `--ui-selftest`'s handles. The paged standalone screen (its own Back, pager,
  `fit()`) is gone, and so is the `--shot` `-keybinds` image (it is `-settings-keys`).
  - **The data is not here.** Bindings live in the shared keybind model — see the KEYBINDS section
    below — and this screen only reads `eden_keybind_*`.
  - **`Button::setRect()` re-wraps only on a width change** (5.4). Every converted screen lays out
    per frame, and until then each button re-rasterised its label — a texture upload, plus N.4.9's
    `glFlush`, per button per frame.
- **Text entry — `GLW::TextField` + the `eden_text_input_*` seam (N.4.5, 2026-10-04).** The first
  **FT.10 (2026-10-27):** field text is Rubik (`FACE_BODY`, pt/1.28 so it reads like the old Jersey size) at a 1.15 em line pitch (`Label::setLead`); a one-line field drops the head of an over-long text so the caret end shows (an over-wide raster used to hit the texture cap and shrink with every character). Scrollbar/slider thumbs draw their top highlight as a fixed 2u strip (FT.12).
  text entry on any port target (`VKeyboard.mm` is a UIKit overlay and is seam-excluded
  everywhere). The field draws itself; the platform supplies composed UTF-8 through six C calls
  declared at the bottom of `GLWidgets.h`: native implements them in
  `port/src/seam/Input_native.cpp` over `SDL_StartTextInput` (which raises the **system keyboard
  on iOS**) and `SDL_EVENT_TEXT_INPUT`, and **while a field has focus every key belongs to it** —
  Escape cancels the edit instead of opening the pause menu, "w" does not walk. Three
  things to know: **the caret is a glyph** (`"name|"`, rasterised with the text — written before the
  raster seam had `eden_text_raster_measure`, when a drawn caret would have drifted); editing is append/backspace only,
  UTF-8-aware and capped in **bytes**; and the platform ending input on its own (the iOS keyboard
  dismissed) is `EV_BLURRED`, distinct from Return (`EV_COMMIT`) and Escape (`EV_CANCEL`).
  `GLDialog::prompt()` is the dialog form of it. **Keyboard avoidance (T.B1, 2026-10-08):** the only
  thing iOS does about an on-screen keyboard is SDL's — it shifts the whole view up until the
  *bottom of the text-input area* meets the keyboard — so a field also takes
  `setKeepVisible(rect)`, which the sixth call (`eden_text_input_keep_visible`) folds into
  that area on iOS only. `GLDialog` passes its whole panel, which keeps its buttons above the
  keyboard; desktops ignore it so the IME candidate window stays anchored on the field.
  FT.6: on iOS the area is clamped by `eden_text_input_clamp_keep` to the field plus ~38 % of the screen height (rows below the field first), because SDL slides the view until the area's bottom meets the keyboard and a taller panel pushed the field off the top.
  FT.5: a strip layout must reset `titlebarHit()` boxes left by the full layout (Back/Menu).
  **C.4 (2026-10-16)** added three calls for a screen that lays itself out instead:
  `eden_text_input_keyboard_top()` (the keyboard's top in point space; iOS only, from a
  `UIKeyboardWillChangeFrameNotification` observer in `port/src/seam/IOSKeyboard_native.mm`),
  `eden_text_input_start_code()` (no autocorrect/autocapitalisation; `TextField::setCodeMode`) and the
  Tab/Up/Down key bytes a code field hands the screen through `takeKey()`. The Console is the one user
  ([console.md](console.md), *The on-screen keyboard*).  - **Rebind capture is armed in the model, not in the screen** (`eden_keybind_capture_begin`).
    The engine's `Input` carries touches and nothing else, so the key that closes a capture arrives
    on a path no GL screen can see: on native it comes through SDL and
    `port/src/seam/Input_native.cpp` feeds it to `eden_keybind_capture_feed()` **ahead of every
    other dispatch**, which is what stops the key you are binding also walking the player. Escape
    cancels (and therefore cannot be bound from this screen — the trade is deliberate: it is the
    one key every keyboard has, and a capture with no exit strands the player in a modal).
  - Only the PRIMARY binding is rebindable. A fixed secondary (arrow keys, the right-hand
    modifiers) is shown as dim "also X" text beside the button rather than on it — visible, because
    it is live input, but plainly not the control.
- `Joystick` (`Classes/Joystick.cpp`) — virtual analog stick, owned by Hud.
- `Graphics::fillScreen` (G.5b-2) — a full-screen `fillRect` in the 2D point space: the HUD's liquid/damage-flash/fade overlays and the menu fade; the crosshair arms are `fillRect`s too. ChatFeed's feed and button push GLW's ortho with `R::begin2D` and pop only the matrices (`hudBegin/hudEnd`), because they draw inside the HUD. **Never nest `GLW::begin2D/end2D` inside `Graphics::beginHud`** (X.31): `kitEnd` sets blend off and depth on, so everything the HUD draws after it (toasts, the version tag, the joystick nub) loses alpha and draws its texture rect opaque. Inside the HUD set the pen like `Hud::renderMenuScreen` (`Pen::blend(true)`, `Pen::blendFunc(R::B_ALPHA)`); the HUD ortho is already GLW's space.
- `Graphics::beginHud/endHud`, `prepareMenu/endMenu` — orthographic projection setup, on `R::begin2D`/`R::end2D` and the renderer's matrix stacks (G.5b-1). Full-screen kit screens open with `GLW::begin2D()`/`end2D()` (`Graphics::kitBegin/kitEnd`; point-space ortho, depth off, alpha blend), not hand-written matrix code; `GLW::fill` draws through `Graphics::fillRect` → `R::draw2D` and states its own blend/depth/cull. The brackets and every other 2D site set the **pen** (G.5b-7, `Pen::` in `Graphics.h`): the colour, blend, depth, cull and texture the ES 1.1 code used to set with `glColor4f`/`glEnable`/`glBlendFunc`/`glBindTexture`, one `Pen::` call per old call, in the same order. Text and icons (`Texture2D` quads) and `Pen::strip` (the Hud's picker faces and block-and-border quads, the GLW icon) draw with it; nothing else reads it. A new 2D site either sets the pen before a `Texture2D` draw or states its own `R::State` and tint on an `R::draw2D`; no UI file calls GL (`port/tools/gl-call-gate.py`).
  All layout code branches on `IS_IPAD`/`IS_WIDESCREEN` with hard-coded coordinates
  in a 480×320 / 568×320 / 1024×768 space.

> **MODIFIED FROM STOCK (2026-07-31) — the point space is no longer a device constant.**
> `SCREEN_WIDTH`/`SCREEN_HEIGHT` are still the layout coordinate system every rect below is written
> in, and every number in this file is still correct *at* 568×320. What changed is that 568×320 is
> now one value among many: the port derives the point space from the real window aspect and a
> UI-scale setting (`port/src/seam/DisplayProfile.cpp`, audit rows D1/D4; first built for the web port), so the same rect
> arithmetic runs at, say, 1024×640 on a desktop window. Three engine-side consequences:
>
> - **The rect math moved out of the constructors.** `Hud::layoutForScreen()` and
>   `Menu::layoutForScreen()` hold everything that reads `SCREEN_*`; the constructors now do state
>   init, build the owned objects (`statusbar`, `Joystick`) and call the layout method once. Both are
>   re-runnable and **must stay idempotent** — `Hud.cpp`'s file-static margins (`marginLeft2`,
>   `marginVert`, `marginLeft`) are mutated by the layout body and are reset at the top of the
>   method so a second call does not compound them.
> - **Edge anchors use the SAFE rect (I.2, 2026-10-17; modified from stock).** `GLW::safeRect()` is
>   the point space less the window's unsafe insets (Dynamic Island, rounded corners, home bar;
>   `eden_safe_rect()` in `DisplayProfile.cpp`), the whole screen where there are none, so every
>   number is stock without insets. `Hud::layoutForScreen` anchors the right-hand mode column,
>   jump/crouch/fly-cycle (render + hit), `rmenu`, the status line, the picker/colour grids (centred on the
>   safe rect; the picker's page strip is clamped above the home bar) and calls
>   `Joystick::layoutForScreen()`, which puts the pad 20 points in from the safe corner (its knob
>   clamps are now pad-relative, and the stick vector's y reads `default_pos.origin.y`: stock read
>   `.x`, equal only while the pad sat at 20,20). `GLW::railFrame()`, `GLDialog`, `WorldBrowser`'s
>   panel, the main-menu window and logo, the Console's keyboard frame, the toast and the version
>   tag frame themselves in it; the pause column centres in it (whether it *fits* is I.4). The world
>   and full-screen scrims stay full-bleed. Gate: `--touch-selftest --device=iphone16`.
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
> native `--display-selftest` (W.2 c; was the web `headless-display-profile-test.js`) pins the stock 568×320 rects byte-exact
> and the idempotence property. At 568×320 every number here is what it always was (the stock iOS
> shell is not built; the native iOS target goes through the same display profile).

## HUD (`Classes/Hud.cpp`, ~2,400 lines)

State machine over `mode`:
`MODE_CAMERA(0)`, `MODE_PICK_BLOCK(1)`, `MODE_BUILD(2)`, `MODE_MINE(3)`,
`MODE_BURN(4)`, `MODE_PAINT(5)`, `MODE_PICK_COLOR(6)` (the `MODE_*` defines in `Hud.h`).

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
  in-game modal; only where `eden_gl_settings_available()`, which answers 1 on every live target —
  the retired web build answered 0). The button gap is kept above 8pt because
  `inbox2`/`inbox3` add 4pt of slop and `handlePickMenu` runs every action whose rect matches.
  The panel no longer fades with `at1` (the kit snaps). (The retired web build gated the
  `renderMenuScreen()` call on a host hook; QA.21 deleted the hook and the legacy-UI switch, so the panel always draws. The corner open-menu icon always draws too, since it is what opens the thing.)
- **Mods page (K.2-5, D13).** After the two stock pages, one page per 35 loaded mod blocks whose pack says
  `"picker": true` (`ContentPack::modReport().blocks`, load order); `Hud::pickerPageCount/pickerCellCount/pickerCell`
  are dynamic, the page dots follow, and a multiplayer session (or no pack) shows only the two stock pages. A mod
  cell is `HUD_MOD_BASE (2000) + index into modReport().blocks`: never a table index (`hud_picker_is_tool` is true
  for it, `hud_picker_is_mod` tells it from SIGN/CMD), drawn by `mod_icon` straight from its `BlockDef` (a
  `missingTexture` block shows the magenta tile). Selecting one stores the **name** (`Hud::modSelection`), never a
  palette id: the id is taken at the first placement by `Hud::resolveModBlock` (called from `Player::processInput`
  before the tool branch), which turns `blocktype` into the world's real id (`BlockRegistry::acquire`; failure text
  such as "palette full" is a status toast). In an unflagged world the first placement asks once per world
  ("Use a mod block?": Place it / Cancel; Cancel places nothing and leaves the file untouched). `worldLoaded()`
  re-points a stale sentinel or resolved id by name (else brick). A hotbar slot keeps a mod block by name (UX.8-3, "Hotbar" below).
  Gate: `--packs-selftest` (pages, tap-select, resolve, re-point, confirm Cancel/OK, session = 2 pages).
- **Block picker** (`renderBlockScreen` / `handlePickBlock`): `NUM_DISPLAY_BLOCKS 35`
  tiles; picking a second block for ramps (`pickSecondBlock`). Sets `blocktype`.
  **Two pages since D.2p (2026-10-10).** Since K.2-3 both pages (and `hudBlocksMap`, each cell's BT second
  block) are data: the stock pack's `"picker_pages"` and each block's `"bt"` (`media/packs/eden/pack.json`,
  [content-packs.md](content-packs.md); `Hud::setPickerPages` at boot), and `picker_page_of` finds a block's
  page by searching page 2. Page 1 is the stock 35 (`hudBlocks[]`); page 2
  (`hudBlocks2[]`, 20 cells in the first 20 `blockBounds[]` slots) is ids 112…127 in **id order**
  (the 2026 game shows 113 first; the user chose id order), then the 1.x flower block `TYPE_GRASS3` (12; H.14, no BT
  variant, so it opens on page 2), then weeds `TYPE_GRASS2` (11; FT.1; no BT variant; its HUD icon is tinted by `blockColor` like the world mesh), then the tools `HUD_TOOL_SIGN` (1000)
  and `HUD_TOOL_CMD` (1001). The tools are **sentinels, not block ids** — `hud_picker_is_tool()`
  guards every table read (`picker_bt()` for `hudBlocksMap`), they draw as pictured cells, and
  `Hud::pickerToolEnabled()` keeps a tool veiled (a tap is consumed, picks nothing) until its row
  turns it on. **SIGN is on since D.3c (2026-10-10) wherever GL text entry exists**
  (`GLDialog::textEntryAvailable()` — every native target, iOS included); **CMD since D.4b**, under the same rule. Both cells (and the HUD's current-block button
  with a tool armed, at 0.6 of the button's short side, centred) show the community art
  (`Resources::getArtTex`, `ART_SIGN_ICON` / `ART_CMD_BLOCK`, 2026-10-27); until then they were
  "SIGN" / "CMD" captions in the body face. Picking SIGN sets `block_paintcolor` to the last picked colour (`paintColor`; 0 = none, so the charcoal default), which `SignTool` reads; CMD clears it. Picking an enabled tool goes through `Hud::update`'s ordinary release
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
  deletes it. Colour = the build paint (`block_paintcolor`, set by tapping build in paint mode or by a hotbar painted slot)
  or 45 (charcoal); an edit recolours only when a paint is picked. Refusals from the trailer
  (full, unreadable) toast through `fm->reportTrailerRefusal`. `pickerPage` + the ◀ ▶ strip (`pickerPrev` /
  `pickerNext`, laid out by `layoutForScreen()` under the grid's last row; hit boxes grow DOWNWARD
  to `touchFloor()` so they never steal the last row's taps) are the only additions: page 1's
  `blockBounds[]` are byte-identical. The window keeps page 1's size on both pages, grown over the
  strip; page dots sit between the arrows. `Hud::pickerPageStep(delta)` (wraps) is the one entry
  point: the arrows, native `[` `]` (`Input_native.cpp`, fixed keys, not keybind rows) and the
  wheel (`eden_ui_take_wheel()` in `Hud::update`). The keys go through `eden_picker_page_step`
  (`Input.cpp`), which answers 0 when the picker is closed so the key keeps its usual meaning. On keyboard + mouse the arrows read `[ <` and `> ]` (UX.8-6 hint; touch keeps `<` `>`). Reopening the picker lands on the current block's page. A page turn keeps a
  pending Block-TNT second pick; no page-2 cell has a BT variant, so all of page 2 is veiled during it.
- **Color picker** (`renderColorPickScreen` / `handlePickColor`): the 54-color grid +
  "no color"; sets `paintColor` (the paint-mode colour, index+1, 0 = none) and enters paint mode; `block_paintcolor`
  (what a placed block carries) is set by `Hud::applyHand` or a painted pick; the pick lands in the hotbar (smart replace, "Hotbar model").
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
  (`Hud::genColorTable`) — a **global** consumed by the mesher; the HUD owns
  the game's color palette, an inversion worth knowing about.
- `MODE_CAMERA`: hides UI, screenshot capture (`take_screenshot` →
  glReadPixels → PNG in Documents + MD5 hash → `FileManager::setImageHash`). Port (H.16): `takeScreenshot()` in
  `Classes/Util.cpp` reads the real drawable size (`eden_gl_context_get_drawable_size`; `R::pickViewport` answers the point space for picking, so never size from it, H.16b), reads the frame with `R::readPixels` (G.5b-3), centre-crops to 3:2, box-downsizes to a long side ≤ 1440 and
  writes `<world>.png` with its own zlib PNG encoder (`photoEncodePNG`; the stock body is under `#if 0`
  because the shim's `UIImagePNGRepresentation` is a nil stub). Gate: `--photo-selftest`. A mouse click in camera mode is the shutter too: `eden_click_begin` forwards it without resolving a tool mode (X.24); `--shot` takes its photo that way.
- Golden-cube counter, health/damage flash (`flash`, `flashcolor`), underwater tint
  (`underLiquid`), FPS counter, fade-in on load (`fade_out`, `justLoaded`).
- `worldLoaded()` resets per-world UI state.

`Hud::update` consumes touches (marks `inuse`) **before** `Player::processInput` sees
them — the ordering in `World::update` is the arbitration.

**Crosshair.** `Hud::render` draws a gapped dark+light cross at the screen centre after `sb->render()` when
Settings › Interface › Crosshair (`eden_get_crosshair()`) is on (default ON on keyboard+mouse, OFF on touch: seeded once from the input profile, FT.13), `mode` is not `MODE_NONE`/`MODE_PICK_BLOCK`/
`MODE_PICK_COLOR`, and no picker is fading (`at2`/`at3`). Camera mode and `hideui` return earlier, so it is hidden there too.

### Block icon (`Classes/BlockIcon.{h,cpp}`, UX.8-1/-2)
The build button's fake-isometric block picture is `BlockIcon::cube(type, colour, Geo, flags)`, a function of
its arguments instead of `Hud` members, so other HUD parts (the hotbar, UX.8) can draw any block. Everything
is in the pass's **raw units** (the HUD's ortho units, i.e. already ×`SCALE_*` on an iPad-scale UI).
- **Cube:** three `Pen::strip` quads (front, top, side), each with its **own face tile** (`blockTypeFaces`
  slots 0/5/3, the unpainted twin when unpainted: grass shows its dirt side) and a flat shade (front .84, top 1,
  side .66). TNT, firework, ladder and `IS_BLOCKTNT` keep one picture on every face (the block-TNT top shows
  its `blockTntMap` block). `FRONT_ONLY` draws the front alone (the Moof).
- **Ramps and sides (`*_RAMP1..4`, `*_SIDE1..4`):** drawn from the world mesher's own triangles
  (`Geometry.cpp` `rampNShortVertices`/`sideNShortVertices` + `*Texture`), projected like the cube (x mirrored:
  the front face is seen from outside) and back-face culled by screen winding, so each orientation looks like the
  placed block. Shade by triangle normal. `Strip(first,…)` indexes vertices and st arrays together: pass st arrays
  as long as the vertex array.
- **`BlockIcon::flat`** (flower, golden cube, door top, portal top: `isFlat`) is the recoloured picture
  (`Resources::getPaintedTex`, 16-entry LRU) scaled into the box with `drawInRect` (which multiplies by
  `SCALE_*` on an iPad: `flat` divides first). Those icon textures load with a world, not in the menu.
- **`cubeMod` / `drawMod(name, colour, rect)`:** a mod block as the same iso cube from its `BlockDef`
  (faces, atlas, tint, `paint_art` ICON); nothing if undefined. The build button's mod branch still draws the
  flat `mod_icon` in `Hud.cpp` (UX.8-4 may switch it).
- **Furniture:** `chip(colour, rect)` (painted block's corner chip, bottom-right), `drawSwatch(colour, rect)`
  (a colour slot's paint chip: keyline, paint, bevel edges; no brush glyph, `UIIcons` has none), `drawEmpty(rect)`
  (dim "+"); plain `Graphics::fillRect` fills.
- It draws no frame: `Hud::renderBlockAndBorder` keeps the build-button art, the tool-sentinel pictures and the
  **stock pixel sizes** it passes as a `Geo` (`size` 29/24/45 by `build_size`, ×2 under `IS_IPAD`).
  `fit(rect)` / `draw(type, colour, rect)` scale the build-size-1 proportions to any box.
  `type` must be a table row (never a tool or mod sentinel); a palette byte with no definition draws the
  magenta missing block.
- UX.8-1 was a pure extraction (byte-identical A/B); **UX.8-2 changes the build button on purpose** (shading,
  side tiles, real ramp/side wedges). Review artefact: `--shot` pages `icons` / `icons-2` (64 px) and
  `icons32` (32 px), drawn full-screen in-world after `hud`: block id 1.. row-major (15 columns at 1136 wide),
  then painted stone/grass/wood ramp/wood side/TNT/flower, two mod-block cells, the missing block, two swatches,
  an empty slot. `TESTERS/hotbar/ux8-2/`. Fences: none exist as geometry in stock (`TYPE_BTFENCE` is a
  block-TNT variant), so there is nothing to special-case.

### Hotbar model (`Classes/Hotbar.{h,cpp}`, UX.8-3)
Nine slots per world; nothing draws them yet (the strip is UX.8-4, touch/pointer interaction UX.8-5). Plan:
`WORKING/hotbar-plan-2026-11-12.md`.
- **A slot** (`HotbarSlot`) is empty, a BLOCK (stock id 1..127, `HUD_TOOL_SIGN/CMD`, or a mod block as `ns:path`, plus
  the `block_paintcolor` a placed block carries) or a COLOUR (1..54, paint mode). Two slots are the same item only if
  kind, type/name and colour all match: painted wood and wood are different items; door and golden cube carry their
  forced colour 20.
- **The hand stays in `Hud`** (`blocktype`, `block_paintcolor`, `paintColor`, `mode`). A slot reaches it only through
  `Hud::applyHand(slot, keepMode)`: BLOCK sets type + colour + `MODE_BUILD`, COLOUR sets `paintColor` + `MODE_PAINT`,
  an open picker is closed (`blocktype_pressed=-1`). A mod slot becomes the pending sentinel `HUD_MOD_BASE+index` and
  the selection name, so a slot never takes a palette id by existing (`resolveModBlock` still does that at the first
  placement). `Hud::handAvailable` says no for a mod block this world has not loaded (or any mod block in a
  multiplayer session), a text tool without text entry, a golden cube with none collected, bedrock: `applyHand` then
  returns false, the hand is unchanged and a mod slot shows a toast.
- **The hand reaches the bar** only through `Hud::noteHand()` (= `Hotbar::noteHand(Hud::handSlot())`), called at the
  three places the hand changes other than a slot select: the picker commit in `Hud::update` (block and colour, on
  the mode change out of the picker), the paint -> build combine on `rbuild`, and the eyedropper
  (`eden_pick_block_at_crosshair`). It is the **smart replace**: the slot that already holds the item is selected;
  else the first empty slot is filled; else the selected slot (slot 1 when none) is replaced. With a **picker
  target** set (`setPickerTarget`, for UX.8-5's re-select) the item lands in the target and, if another slot held
  it, the two swap. A new place that changes the hand must call `noteHand()`.
- `Hotbar::select(i)` (`eden_select_hotbar_slot`; the keys go through `eden_hotbar_key`, see UX.8-5) refuses an empty slot; `scroll(dir)` (wheel, pad
  shoulders, d-pad left/right) steps to the next slot `handAvailable` accepts, wrapping. `selected()` is -1 when the
  hand matches no slot.
- **Persistence** is the sidecar `Worlds/<id>/hotbar.json` (`docs/save-load.md`), read in
  `FileManager::finishLoadOnMain`, written by `saveIfDirty()` after a world save and in `World::exitToMenu`. A bar
  read from the world's own file puts its selected slot back in the hand with the mode kept; a world without one
  starts from prefs `hotbar.last` (the last bar saved anywhere) minus mod slots, else the stock nine, and never
  changes the hand, so a fresh world's HUD is as before.
- Gate: `--hotbar-selftest` (`port/src/harness/mode_hotbar.cpp`).

### Hotbar strip (`Hud::renderHotbar`, UX.8-4)
- **Setting** Settings > Gameplay > *Hotbar* (`hotbar`, `eden_get_hotbar_shown()`): seeded on for keyboard+mouse, off on
  touch. Off = zero draws, no toast lift (HUD shots byte-identical to before); the 1-9 keys work either way.
- **Layout** (point space, drawn after the joystick/chat button and before the toast): desktop = bottom centre, cells
  `du(40)`, `du(8)` above the safe bottom, and `GLW::Toast::setLift` raises the status toast above it. Touch = top
  centre between the version tag (tag end taken as safe-left + 121, or + 175 with the chat/console button) and
  `HUDR_X - 8`; cell 44 pt shrinking to a 36 pt floor, else a 7- then 5-cell window centred on the selection (`<` `>`
  at an end mean more slots). Sized from the free width, never from the profile name.
- **Look**: dark plate, 2u `#646464` keyline, 2u hard shadow; the selected cell is lifted 2u with a 3u lime ring
  (dim lime while the hand is mine/burn). Icons are `BlockIcon` (block + colour chip, swatch, tool art, dim `+`);
  a slot `handAvailable` refuses is dimmed. Desktop cells carry the live binding of `hotbarN` as a key hint.
- **Name label** 1.5 s after a selection change (not on the first frame of a world), above (desktop) / below (touch)
  the strip: `Hotbar::label` ("Stone ramp 1", "Sign", "Colour 12", painted adds " (colour N)").
- **Hidden** with F1, camera mode, the pause menu, and under a picker when the picker panel (`rpaintframe`)
  intersects it (always, on the touch top strip).

### Hotbar interaction (UX.8-5)
- **One layout, two users**: `Hud::hotbarLayout` (n, window start, cell, left, y) feeds both `renderHotbar` and
  `Hud::hotbarButton(idx)` (0-8 slot cells, 9/10 chevrons; zero-size when hidden or outside the window). Cells abut (each takes
  half the 2u gap either side, so no two overlap); on touch a cell is at least `touchFloor()` tall, hanging from the top edge.
- **Claim**: a touch that goes down on a cell or chevron (not in the pause menu) gets `inuse = usage_hb (17)`, so the player
  never places a block or turns the camera with it. Desktop: only reachable with the cursor free (a picker open, strip not
  hidden by it); a captured mouse never produces HUD touches.
- **Tap** (release, no drag): `Hud::hotbarActivate(slot)` = a picker open: retarget it to that slot; the *selected* slot
  while the hand is in use, or an *empty* slot: open the block picker for it (the colour picker for a colour slot) with
  `Hotbar::setPickerTarget` and the status line "Choosing for slot N"; else `Hotbar::select`. The 1-9 keys take the same
  route (`eden_hotbar_key`). A select by any route plays `S_MENU_BUTTON_PRESS` once (`Hotbar::select`).
- **Pick lands in the target**: the picker commit goes through `noteHand` as before (swap with the slot that held it). Closing the
  picker any way (commit, Escape, menu) drops the target and the status line (`Hud::update`, `s_hbChoosing`).
- **Assign by number**: with a picker open and the mouse over a cell, 1-9 assigns that cell to the slot
  (`Hud::hotbarAssignAt` -> `Hotbar::assign`: an item already on the bar swaps places instead of duplicating); the picker stays
  open. Cells that are not items (block-TNT's two-step, `TYPE_CUSTOM`, a tool whose row is off, a golden cube you do not own) refuse.
- **Clear**: long-press (0.5 s, no drag) on a cell, or right-click on one with the cursor free (`eden_hotbar_right_click`);
  `Hotbar::clear`, status "Slot N cleared".
- **Slide** (window < 9 cells): a horizontal drag over the strip moves the window one slot per cell pitch and never selects;
  a chevron tap slides one window. The slide is forgotten when the selection changes (the window re-centres).
- Test hooks: `eden_tap_hud_button_begin/end` ids 11-19 (cells), 20/21 (chevrons); `eden_debug_hotbar_rect/overlaps`;
  `--touch-selftest` (tap, re-tap, pick, long-press, safe rect, overlaps) at `--device=iphone16`, `iphonese` and
  `--window=1024x768 --touch-profile`; `--hotbar-selftest` (assign/clear/activate).

## Menu system (`Classes/Menu.cpp` + satellites)

`Menu` is active in `GAME_MODE_MENU`; `activate()/deactivate()` load/free its textures
(the retina-scale juggling in `World::exitToMenu` wraps `activate`).

- **World list**: `loadWorlds()` lists the Documents directory, reads each file's
  header for the display name (`FileManager::getName`; files returning `error~` are
  skipped), builds a doubly-linked `WorldNode` list (`Util.h`; since G.2b-2 its `display_name`/`file_name`
  are `std::string`, so a node is `new WorldNode()` / `delete`, never malloc/memset/free). Stock drew it as a carousel of atlas blocks
  with arrows and corner icons (create, delete, share, get worlds, options); tap a block to select,
  tap again to load. Create (with `VKeyboard` name entry; flat-vs-default via `a_genFlat`), delete
  (confirm via `Alert`), rename.
  **Stage 5.6 (2026-10-06, modified from stock): the screen is `GLWidgets`.** Under the logo (art,
  kept) a WINDOW holds a titlebar (Settings · "Worlds" · New — which since U.5 opens `NewWorldMenu`, below), the list as `GLW::ListRow`s in a
  row-snapped `GLW::ScrollView` (drag, scrollbar, wheel) and an action bar (Delete · Rename (Properties
  since H.15) · Play, Play in the positive tone); `sbar`'s message is drawn by the kit under the window (in-game
  chrome's light-on-dark), and `fnbar`'s job — naming the selection — is the selected row's.
  `layoutKit()` runs per frame from `SCREEN_*` and the list length: rows at `max(30u,
  touchFloor())`, at least four, at most what fits, and the window shrink-wraps them. **The state
  and the actions are stock:** `selected_world`, tap-the-selected-row-to-play, the create branch,
  `loading`'s ladder in `render()`, `showAlertDeleteConfirm` → `a_deleteConfirm`, `showsettings`
  — so the `Menu.cpp` seam accessors are unaffected. The stock rects (`rect_options`,
  `rect_create`, the arrows …) are still computed by `layoutForScreen()` and nothing reads them.
  **Share** (`kit_share`, beside Rename; **S.5, 2026-10-10**, native only — `WorldShare::offered`)
  opens `Classes/WorldShare.{h,mm}`'s GLDialog chain: **Export** / **Upload** / **Remove original**
  (only for an `.emod` with its `.eden` kept) → a format picker (Automatic = own format, first; Legacy64z,
  NewDawn256z, NewFormat256z) → the signs question, only when pruning would drop any → a confirm
  showing the pre-flight size and loss sentence → a time-sliced job on the status bar. Export
  writes `Documents/Exports/<name>.eden.gz`; Upload is networking.md § Upload. The stock
  `rect_share`/ShareMenu flow stays unused. `WorldShare::startExport/startUpload/lastResult` skip
  the dialogs for `--browser-selftest`. **Get Worlds is,
  since Stage 5.9 (2026-10-07)**, wherever the host can fetch (`Menu::browserOffered()` =
  `WorldBrowser::available()`, false only with the `NetNone` backend): a titlebar button beside New (the "Worlds" title
  yields when the two would collide) opening `WorldBrowser` — below; since I.7 a tile on the home screen. `controlRect(name)` (incl.
  `"getworlds"`)/`rowRect(i)` exist for the harness.
  **About (LG.8, 2026-10-25).** A small **About** button sits bottom-left of the home screen (the panel is centred above it);
  it sets `Menu::showabout`, handled before `showhome` in `update`/`render` (`layoutAbout`/`renderAbout`/`updateAbout`). It is a
  plain placeholder: a gray window, one `GLW::Label` per paragraph (text in `kAboutText`, Menu.cpp: fan-project disclaimer,
  GPLv3 notice, Kevin MacLeod CC BY 4.0 credit, third-party list), font shrunk until it fits, Back button. Keep it in step with `NOTICE`.

  **The home screen (I.7, 2026-10-17, modified from stock).** Boot lands on a home screen
  (`Menu::showhome`, `layoutHome`/`renderHome`/`updateHome`, kit state in `MenuKit`): the logo (it
  left the list) and under it a gray `BEVEL_WINDOW` panel (`MenuKit::homePanel`, the surface the list
  sits in) headed "Community Edition" in kit text: a two-column grid of RAISED text buttons —
  **My Worlds** (green, `TONE_POSITIVE`) → the list above and **Get Worlds** (cyan, `TONE_INFO`;
  disabled where `browserOffered()` is false) as the tall row, **Multiplayer** (since MP.6 opens the
  server list, below) and **Settings** under them. The panel is centred in the space under the logo; on a short
  window the tall row flattens and the padding tightens first. Tile labels never wrap: when the wider
  word of a pair does not fit, both shrink to it (`layoutHome`, redone only when the tile width
  changes). (The MENU_* art tiles of the first I.7 cut are gone; `menu_home_tile` is unused.) The list ("My Worlds"
  titlebar: **Back** · title · New) has no logo and fills the safe rect's height; Settings and Get
  Worlds left it. Back or Escape (`Menu::back()`'s fallback when no screen is open)
  returns home; Escape on home does nothing. **Quit to Menu** (`Menu::activate`) and a finished Get
  Worlds download (`addImported`) land on the list; the browser's Back returns to home. Harness keys
  `"home.myworlds"`, `"home.multiplayer"`, `"home.getworlds"`, `"home.settings"`, `"back"`; the old
  `"settings"`/`"getworlds"` answer the home's controls.
  **Multiplayer screens (MP.6, 2026-10-17, new):** `ServerListMenu` (usage_id 19, `Menu::servers`/`showservers`/
  `openServers()`; updates/renders in the browser's slot, so the loading ladder still draws) is a `railFrame` list of
  two-line rows (label + MOTD/address/status, players, ping bars + ms, `UIICON_LOCK`/`LOCK_OPEN`, "?" when unknown), a
  detail pane when the area is ≥ 300 pt tall and ≥ du(560) wide (desktop/iPad, not phones), a bar ("Playing as …" ·
  Edit · Refresh · Join) and an empty state. `ServerFormMenu` (usage_id 18) is Add/Edit server (Name, Address, Password
  secure, Remember; foot Delete · Move up/down · Join now) and Profile (name, Character stepper) with live red/amber
  hints; **Back / Escape asks "Discard changes?" only when a field differs from what the form opened with** (`dirty()`, a snapshot taken in `open()`; Save-enabled alone would nag on an unedited Edit); with a keyboard up the foot hides and the focused row is the lowest shown. The Connecting window and the
  password/error prompts are `GLDialog`s (`setPromptSecure`). `Menu::activate` reopens the list when a session ends,
  and `Menu::update` opens it when a join is in progress (`--mp-join`). Remote text is drawn by the ASCII-only raster,
  so separators are ", " and bullets '*'. Harness: `controlRect`/`rowRect`/`debugConnecting`/`debugError`/`debugPassword`.
  **Native delete actually deletes since 5.6**: its `showAlertDeleteConfirm()` was a no-op ("not
  confirming is the safe default"), so the stock button did nothing at all; it is now a
  `GLDialog` (Delete · Cancel).
  **Rename on the port (N.4.5, modified from stock):** a kit `Rename` button in the action bar
  (`Menu::renameOffered()` — hidden in delete/share mode and while loading) opens
  `GLDialog::prompt`; its commit is `Menu::renameSelected()`, which trims, rejects an empty name and
  calls `FileManager::renameWorld()` — a new `WORLD_HEADER` record for an `.emod` (an in-place
  50-byte `WorldFileHeader::name` rewrite for a leftover `.eden`; a never-played world has no file
  and needs no write).
  **Since H.15 (2026-10-15) the button is Properties** (same slot, `rect_rename`,
  `controlRect("properties")` = `"rename"`, `Menu::propertiesOffered()` — no text-entry requirement)
  and opens `WorldPropertiesMenu` (below); the name is that screen's first field. `beginRename()` /
  `renameSelected()` remain (the harness, an `.eden`'s name path) but nothing in the list reaches the
  prompt. `FileManager::renameWorld()` now **refuses** a dropped-in `.eden.gz` / `.zip`: its header is
  inside the deflate stream, and the in-place write used to land 50 raw bytes in it.
  **The list is `FileManager::listWorlds` (K.0w-4):** one folder per world under `Documents/Worlds/`, last played
  first (mtime desc, ties by key), then unconverted dropped sources by name; the photo is the folder's `photo.png`.
  **An empty Documents folder means an empty list** — `selected_world` is `NULL` and every
  deref reachable from that state is guarded (`refreshfn`, `activate`, the layout block).
  Stock behaviour was to synthesise one placeholder `WorldNode` so the carousel was never
  empty; that was removed 2026-08-06 because a placeholder is indistinguishable from a saved
  world but has no file behind it, so tapping it lands in `loadWorld()`'s create-a-world branch
  and parks on the Flat/Normal question — which the player reads as "loading forever".
- **`NewWorldMenu`** (`Classes/NewWorldMenu.{h,mm}`) — **Stage U.5 (2026-10-14), new; modified from
  stock.** The main menu's **New** opens it (`Menu::openNewWorld()`, `shownewworld`, drawn and updated in
  front of the menu like Settings and Get Worlds). Stock New added "World N" to the list at once and
  asked Flat / Normal with a modal when it was first played; the port's native seam added a second modal
  for the height (S.5e). The screen asks all three up front, on Settings' layout: the shared fixed
  panel (`GLW::railFrame()`, lifted out of `SettingsMenu::layout` so the two cannot drift), Back · "New
  *Type* World" · **▶ Play** (positive tone) in the titlebar, a `GLW::IconRail` of world types (Flat ·
  Normal = `FileManager::genflat`; the first visit opens on Normal, later visits keep the last type and
  height), and the options as `TwoLineRow`s in a variable-height `ScrollView`: **World name** (a
  `GLW::TextField`, 48 bytes — `FileManager`'s header writer uses `getCString:maxLength:49`, which counts
  the terminator; the placeholder is the "World N" a blank name becomes, peeked from
  `SettingsMenu::world_counter` so Back burns no number) and **Height** (New Dawn 256 · Original 64 —
  "Classic 64" until H.4 took the word for the generator — a pressed-is-selected pair, 256 first and default). The name field takes focus on open under a pointer,
  never on touch (the system keyboard would cover the screen unasked); a tap on the field (on touch, on
  its row) focuses it, any other release blurs it; Return in it is Play. **Play** trims the name (blank →
  `getNewWorldName()`), parks type and height in the one-shot pending seam (`Menu.cpp`'s
  `eden_menu_set_pending_world_type/height`), and calls `Menu::startNewWorld(name)` — stock New's
  create body plus `loading=1` — so the `loading` ladder's `showAlertWorldType()` and
  `FileManager::probeWorldHeight()` take the parked answers and **no dialog shows**. **Back creates
  nothing**; **Escape** is a row of `Menu::back()` (QA.22, `Menu.cpp` `kBackScreens`) — while the field has focus the text seam gives Escape to the field, so the first one only leaves
  it. The dialogs stay for a world made any other way (`eden_menu_create_world()`, a harness).
  **Room for Stage H:** types and options are tables (`kTypes[]`, `kRows[]` with a per-type mask), so the
  seeded generator's seed/caves/presets are rows and a third generator is a tile; a new row KIND (a
  toggle, a stepper) is the only thing that owes code. Harness: `controlRect("back"|"play"|"name")`,
  `typeRect(t)`, `choiceRect(key, option, &r)`, `choice(key)`, `text(key)`, `showType(t)`.
  **H.4 (2026-10-15) filled that room:** a third tile, **Classic** (`UIICON_TREES`), and four row kinds
  — `NW_SEED` (a `TextField` + a shrink-wrapped **Random** button; the rolled seed is the placeholder,
  re-rolled per visit and by Random; `seedFromText()`: digits are the number, other text is FNV-1a,
  both reduced into [1, 2^24) and never 333333), `NW_SLIDER` (**Scale**, a detented `GLW::Slider` over
  1×/1.5×/2×/3×/4×, its hint naming the level; **hidden, not greyed, at Original 64** — a height tap
  rebuilds the row list), `NW_TOGGLE` (Flowers, Clouds) and `NW_PREVIEW` (a 128×128 RGBA `Texture2D`
  of `classic_height()` hillshaded, one sample every 4·s blocks around chunk 4096, a white cross at
  spawn; rebuilt only when seed/scale/hilliness/height change, ~10 ms). Hilliness, Caves (Off · 2011 ·
  Deep), Trees and Size are `NW_CHOICE` rows with a per-row default (`NWRow::def`); `NW_MAX_OPTS` is 5.
  Play on Classic encodes the tab (`classicSettings()` → `classic_encode_record()`) and parks it with
  `eden_menu_set_pending_generator()` beside the type ("normal") and height; any other type parks NULL.
  Harness additions: `controlRect("dice"|"seed"|"preview")`, `rowVisible(key)`, `toggleRect`/`toggle`,
  `sliderRect`, `seed()`, `classicSettings()`; shots `newworld-classic`, `newworld-classic-64`.
- **`WorldPropertiesMenu`** (`Classes/WorldPropertiesMenu.{h,mm}`, `Classes/WorldProps.h`) — **Stage H /
  H.15 (2026-10-15), new.** The world list's **Properties** (Rename's slot) opens it for the selected world
  (`Menu::openProperties()`, `showprops`, drawn and updated in front of the menu like New World; Escape is
  Back through `Menu::back()`). New World's frame (`GLW::railFrame`): titlebar **Back ·
  "‹name› Properties" · Save** (positive tone, disabled until something differs), rail tiles **World**
  (`UIICON_INFO`) and **Terrain** (`UIICON_MOUNTAIN`), `TwoLineRow`s in a variable-height `ScrollView`.
  Its own `usage_id` 14. Rows are built per world at `open()` (`buildRows()`), not from a static table:
  - *World*: first a photo row (`WPK_PHOTO`, no `TwoLineRow`) — the world's last in-game photo,
    `Documents/<world file>.png` (camera mode, H.16), aspect-fit in a 3:2 sunken frame, loaded once with
    `Texture2D(path)` and sized from its IHDR as Get Worlds' preview is; none → a grey frame, *"No picture
    yet. Take one in camera mode."* Beside it the name, height, type (Flat / Normal / Classic / Classic
    (1.x) / Classic (newer Emod)) and file size. Then the name field (48 bytes, New World's rule; blank
    keeps the name; read-only inside an archive), and read-only `WPK_VALUE` rows (right-hand text): Seed
    (Classic), Height, Saved land (columns), Signs, Golden cubes held, Created / Last saved (the `.emod`
    file header's `created_unix` / the file's mtime), Converted from (`PROVENANCE`), Original kept (S.5).
  - *Terrain*: a Classic world gets a lead row (what a change does) and New World's Classic rows over
    the same option tables (`Classes/ClassicOptions.h`, shared since H.15): Hilliness, Caves (a detected
    legacy value says so in its hint), Trees, Flowers, Clouds, Size, and Scale — a slider on a new 256z
    world, a read-only value on a 1.x one, hidden at 64. Flat / Normal / a record from a newer Emod show
    one type row; an unconverted `.eden` says to play it once first. Seed and height are never editable.
  **Saving:** a name-only change saves at once; a terrain change raises a `GLDialog` (**Change terrain** /
  Cancel) first. Both go through `FileManager::applyWorldProperties()` (save-load.md), then the list entry
  is renamed in place, the status line says *Saved* and the screen closes; a failure is a one-button
  dialog and the screen stays. **Back / Escape** close at once when Save is disabled, else ask "Discard changes?" (Discard / Keep editing; UX.9). Harness surface: `controlRect`
  (`back`/`save`/`name`/`photo`), `tabRect`, `choiceRect`, `toggleRect`, `sliderRect`, `value()`,
  `rowVisible()`/`rowEditable()`, `lastResult()`; gates `--props-selftest` and the Properties cases of
  `--ui-selftest`; `--shot` writes `props-world-nophoto`, `props-world` and `props-terrain`.
  Plan: `WORKING/feature-archaeology-2026-10-13/world-properties-plan.md`.
- **`Menu_background`** — the animated menu backdrop.
- **`SettingsMenu`** — the GL settings screen. **QA.19 (2026-11-13):** the stock 2.1 toggles (Health/Autojump/
  Creatures/Music/Sound) are ordinary rows of `kSettings[]`, persisted under their stock prefs names as 0/1 ints;
  `properties[]`, `NUM_PROP`, `rect_on[]` and `save()` are gone, and `load()` runs once at construction (world
  counter, `eden_settings_boot_stock()`, boot values of `invertcam`/`use_joystick`/`genCaves`/`bestGraphics`).
  `LOW_MEM_DEVICE`'s old "Creatures default off" went with `properties[]`: the flag is never assigned (N.6-2b). **`render()`/`update()` were rewritten in
  Phase N Stage 2.5** into a generic paginated list over the shared settings table
  (`port/src/seam/Settings.cpp`'s `kSettings[]`, reached via `eden_settings_*` scalar
  accessors; U.2 added `eden_settings_hint(i)` and `eden_settings_parent(i)` — the trailing `Setting::parent` key of
  a toggle that mutes the row while off, -1 if none; the schema JSON carries `parent`). U.2 gave every row a hint;
  **U.4 trimmed them** (the user, 2026-10-12: brief, and only where the label does not already say it) — a row
  may have none (`NULL` → `""`, and `TwoLineRow` drops the second line).
  Interface → Video is a screen-side mapping (U.3): the `group` strings and persistence keys are unchanged.
  **Stage U.3 (2026-10-12) rebuilt the screen on the reference layout** (`WORKING/ui-rework-plan-2026-10-11.md`
  §3) — what this paragraph describes after the history below is the current shape: a **fixed panel** (the
  783×587u design frame less a 12u margin, centred, the same size on every tab; since U.5 computed by the
  kit's `GLW::railFrame()`, shared with New World); a titlebar of a shrink-wrapped
  Back and "*Tab* Settings" centred on the window; a `GLW::IconRail` of **7 tabs** (Gameplay · Audio · Controls ·
  Keys · Video · Experiments · Saves; `kTabs[]` in the .mm, a tab with no native rows is a disabled tile); the
  tab's rows as `GLW::TwoLineRow`s (title + hint) on one CONTENT surface in a variable-height, row-snapped
  `GLW::ScrollView` (wheel, content drag, scrollbar); a row with a `parent` is indented 14u and **muted while that
  toggle is off** — display-only, it still works (SX.2: toggle, slider readout, stepper and text/page button all grey their label or value via `setMuted`; `Button::setMuted`/`Stepper::setMuted` leave chrome and hit-testing alone). Slider readouts print 2 decimals when the step is < 1 or the default is fractional. Control column: the 104×34u toggle, a 200u slider + readout, a
  168u `<` value `>` stepper; on the touch profile a toggle's hit box is the whole row. Labels are built on a tab
  switch or a `u()` change, never per frame. **Keys has no settings rows**: its content is the embedded
  `KeybindsMenu` (U.4, above) and its titlebar gains Defaults. The pages, the pager, `fit()`'s pointer/touch pitch split and the group headings are
  gone. **Escape** reaches it as `eden_settings_menu_back()` (`Settings.cpp`, called from `Input_native.cpp`'s
  menu key) → `SettingsMenu::back()`: closes the screen (an armed key capture never sees it — the keybind
  model consumes Escape as "cancel"). It used to arrive as a synthetic tap on
  the HUD's menu button, which this screen claimed as one of its own touches and dropped ("Escape inside in-game
  Settings is swallowed"). Harness: `tabCount()/showTab()/tabName()/tabRowCount()` and `showRow(key, &rect)`
  (switches tab, scrolls the row into view).
  **Sub-pages (SX.1, 2026-11-06; `WORKING/settings-ux-audit-2026-11-06.md` §4.1)** are data: `kPages[]` in
  `SettingsMenu.cpp` (`{id, title, parent tab, shot, reset}`), no rail tile. A `KIND_PAGE` row (kind 5, an "Open"
  button; its `options` is the page id) opens one; the rail keeps the parent selected, the title reads
  "*Parent* > *Page*" (ASCII: the fonts lack U+203A), and **the titlebar Back and Escape go up to the parent tab**,
  scrolled to the opening row (`upLevel()`); only on a tab do they close the screen. A page's `reset` is its
  titlebar **Defaults** action (as on Keys). A row lands on a page by the trailing `Setting::page` field, which is
  **display placement only**: the console name stays `lower(group).key`, so moving a row never renames its cvar.
  `page` may also name another tab's group. `KIND_HEADING` (kind 6) is a label-only, muted, control-less row for
  sections. Neither kind holds a value, persists or becomes a cvar (`bridgeSettings` skips kind >= 4). Views are
  indexed tabs first, then pages: `tabCount()` = 7 + pages, `railTabCount()` = 7, `currentTab()`, and a page's
  `tabName()` is `<tab>-<page>` (`--shot` writes `settings-experiments-srclike`, `-experiments-online`, `-video-atmosphere`, `-world-weather`).
  **SX.4 layout** (audit `WORKING/settings-ux-audit-2026-11-06.md` §3): rail = Gameplay, Audio, Controls, Keys, Video,
  Experiments, **World** (was Saves; day/night, weather and the three save rows, `page "World"`, under *All worlds* / *Saves*
  headings). Video has *Quality* / *View* / *Comfort* headings. Gameplay keeps Allow fly, the four fly sliders and the new *Noclip speed* (`NOCLIP_SPEED`, cvar `gameplay.noclip_speed`) inline (no page: they fit). Pages: Experiments > *Srclike settings*
  (`srclike`: auto-bhop, Srclike sensitivity, the SRC.8b sliders; renamed from Movement tuning) and Experiments > *Online sources*
  (`online`: Matchmakers, Get Mods catalogue). *Crouch mode* is on Gameplay and *Camera view* on Experiments by `page` (cvars
  `experiments.crouch`, `video.camera_view` unchanged). The *Mods* row is gone (the home menu has the button). Gate: `--ui-selftest`
  (Srclike settings legs: sliders, Defaults, Back/Escape up a level).
  **SX.6 (2026-11-07; audit §4.2–4.3): cvar rows, scope, This world.** A `KIND_CVAR` row (kind 7; `options` = a console
  variable's name) shows a value owned elsewhere: `SettingsMenu`'s `sm_*` wrappers draw it as the cvar's own kind (toggle,
  range, enum) with the cvar's range/options and read/write through its `get`/`set` (`Console::init()` on first use;
  an unregistered name draws as a heading and logs once). Its shown values are re-read every frame (the clock runs);
  `refreshValue` re-rasters a readout only when its text changed (`m_valueText`). It holds no value, never persists and is
  never re-bridged (`bridgeSettings` skips kind >= 4). **The rule:** a value that persists in `eden_prefs` is an ordinary
  row whose cvar is derived from it (`eden_settings_cvar(i)`: `lower(group).key`, or `move.<key less move_>` for group
  "Movement tuning"; `bridgeSettings` and `--console-selftest` gate 3 both use it); a value saved elsewhere (the world's
  `.emod`) is a cvar shown by a `KIND_CVAR` row. Trailing `Setting::scope`: 0 always, 1 = only with a world in play
  (`terrain->loaded`), 2 = only without; `build()` filters by it and `layout()` rebuilds when the world opens/closes or a
  multiplayer session starts/ends. World tab: *All worlds* gains *Weather tuning* (page `weathertune`: `snowline`,
  `mistheight`, group `Weather` = cvars `weather.snowline`/`weather.mistheight`, now persisted for all worlds; titlebar
  Defaults `eden_weather_tuning_reset`). Beside it, *Weather frequency* (Rare/Normal/Often) and *Storms* (Off/Rare/Normal/Often): group `Weather` enums = cvars `weather.frequency`/`weather.storms`, applied through `Weather::setFrequency`/`setStorms` (SX.9), muted under the *Weather* toggle. *This world* (scope 1): *Time of day* (`world.time`, readout a 24-hour clock),
  *Lock time of day* (`world.locktime`, muted under Day/night cycle), *Weather now* (`weather.now`: Clear, Overcast, Rain,
  Storm, Snow) and *Lock weather* (`weather.lock`), both muted under Weather; saved with the world at its next save
  (`SKY_CLOCK`, `WEATHER`). On the title screen a scope-2 heading says "Open a world to change its time and weather". In a
  multiplayer session (U3) the section stays and its heading's hint becomes "Temporary: this device only..." (`sm_hint`;
  the rows write local state only). `rowHint(key)` is the harness accessor. Gate: `--ui-selftest` (title-screen scope;
  in a world: time, weather, both locks through the controls, quit, reopen, read back via the cvars; the badge with
  `fm->sessionWorld` set). *History:* **Stage 5.4 (2026-10-04) reskinned it onto `GLWidgets`**: a WINDOW panel with a
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
  `eden_settings_loaded()`. (Until U.3, `pageCount()/showPage()` were the harness's entry points.) Rows
  whose effect was web-only (`ui_scale`, `display_layout`, `input_mode`) are hidden via `eden_settings_native_hidden()` (the retired web build `--wrap`ped this
  screen away and drew a DOM panel). `render_scale` was on that list
  until N.4.11 (2026-10-04); native now renders the 3D pass at it (see rendering.md, "Scene-pass
  bracket"), so the Video page shows it. It was hidden on iOS from 2026-10-09 (N.4.12, the composite
  did not land) until FT.16 (2026-10-28) composited by a textured quad; iOS offers 50/75/100% and seeds 75%
  on Air 2-class devices (`eden_ios_air2_class`, `hw.machine`).
  **Stage 5.3 added the keybinds entry** (the titlebar's "Controls" button from 5.4, the Keys tile since U.3); since
  U.4 it is the Keys tab's content rather than a screen of its own.
- **`WorldBrowser`** (`Classes/WorldBrowser.{h,mm}`, **added Stage 5.9, 2026-10-07**) — "Get
  Worlds" on the kit, replacing `SharedList` on the port targets (where SharedList/ShareUtil are
  seam-excluded NSURLConnection code). One `TabRail` tab per SOURCE: **Archive** (the community's
  `hagg3.github.io/edenarchive` manifest), **Current
  server** (`app2`/`files2.edengame.net`) and **Legacy server** (`app`/`files.edengame.net`). The
  servers keep stock's modes as a second rail — Featured (`popularlist.txt`) / Recent
  (`list2.php?start=N&sort=2`, paged by **More**) — plus server-side Search on Return; the archive
  filters its whole catalogue as you type (name, author, tags). A row tap selects (a tap on the
  selection downloads, as the menu plays); the detail pane shows the preview (`<id>.eden.png`,
  fetched 0.25 s after the selection settles), the date (an official id IS the upload's unix time),
  author / size / tags where the archive has them, and the download's progress. **Download** streams
  through the net seam to `Worlds/.<id>.partial/download-N` (K.0w-3a; pre-K.0w: `<saves>/.emod-download(.part)`) (preview: `.emod-preview.png`), then **unpacks a few MB per frame** over
  zlib — gzip (multi-member), a single-entry zip (central directory trusted over a streamed local
  header) and the archive's zip-in-a-zip, up to four layers — and converts as it unpacks (S.5:
  `emod::ImportJob`, `DL_CONVERT`; no inflated `.eden` is written; an uncompressed payload must
  describe itself, `WorldFileHeader::directory_offset` inside the file). The world lands as
  `<id>.emod` or `<id>-N.emod` (never replacing a world the player already has; 64z worlds upgrade
  per Settings › World › Saves `upgrade_256z`, no prompt), is named from its own header, added as a
  `WorldNode` and selected, the download is deleted, and the browser hands back to the menu. Ids that are not
  `[A-Za-z0-9_-]` drop their entry (they become paths); names are UTF-8-cleaned before they become
  textures. Back cancels an in-flight download. Each source keeps its list, scroll, selection and
  query across tab switches and across closing the screen. **The HTTP stack is the host's** —
  `eden_net_*` (declared in `WorldBrowser.h`): `port/src/seam/Net_native.cpp` over AppleNet (NSURLSession) /
  libcurl / WinHTTP, `native/README.md`), or `NetNone` (no network: Get Worlds is not offered).
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
  `statusbar::current()` (5.6) returns the live message for a screen that draws it in the kit
  (a `const std::string*` since G.2b-2; null when nothing shows).

### The Console screen (Stage C / C.3)

`Classes/ConsoleMenu` is the one screen that runs over a **running** world: it is `Menu::console` /
`showconsole`, updated first in `World::update`'s play step and drawn after the HUD, and it claims every
touch while the play step goes on. Its layout is two: the `railFrame()` panel (Console / Variables / Help
tiles, `UIICON_TERMINAL` is the Console tile) and a top-docked strip. The singleplayer HUD console button (`Hud::rcons`, beside the gear where chat sits in a session; shown only with
Settings › Experiments › *Allow developer console*, `DEV_CONSOLE_ALLOWED`, default off) shows
`ART_CONSOLE` (console_button.png); the pause menu has no Console entry (its `PM_CONSOLE` slot is always hidden). Settings shows `ICO_OPEN_MENU` (the HUD's top-left gears), drawn like Save /
Home / Photo / Quit's stock icons (2026-10-27; Console had the `UIICON_TERMINAL` glyph before). The
pause menu has seven buttons now (`PM_CONSOLE` before Quit); I.4: when one column exceeds the safe rect (a phone) `Hud::layoutPauseMenu` lays out Resume full-width then pairs. See [console.md](console.md) for the
behaviour, the routing (`eden_console_menu_*`, `eden_ui_captures_keys`) and the cost notes.

**MP.7 (a multiplayer session):** the rail gains **Chat** and **Players** tiles in front (`UIICON_MESSAGE_SQUARE`, `UIICON_USERS`; the UI atlas is
now 512×512, 8×8 cells), tab ids `TAB_CHAT = 3`, `TAB_PLAYERS = 4` appended, rail order mapped by `tabOfTile`/`tileOfTab`. Chat is the
Console tab over `ChatFeed`'s lines (`srcLines()`; the label cache is flushed when the source changes) and `openChat()` is its entry
(`eden_chat_open`: T, `/`, the HUD's `rchat` button). The HUD feed and the button are `ChatFeed::renderHud` / `renderButton`, drawn from
`Hud::render`; `Hud::rchat` is claimed on the down edge like `rmenu`. Details: [networking.md](networking.md) "Chat".

**MP.8 / the home screen:** the home **Multiplayer** tile wears an amber **BETA** pill straddling its top-right corner (`Menu::renderHome`,
`MenuKit::betaTag`). The server list's titlebar swaps its title for a `TabRail` (My servers · Matchmakers) when Experiments › Matchmakers
has addresses ([networking.md](networking.md) "Matchmakers"). `SettingsMenu` draws a `KIND_TEXT` row as a button that opens a prompt.

### The Mods screen (K.3-5 / K.3-6)

`Classes/ModsMenu` (`Menu::mods` / `showmods`, `Menu::openMods()`; updated and drawn in the slot of `showbrowser`, touch claim id 20) is
WorldBrowser's shape: Back, title, an optional `TabRail`, list + scrollbar, detail pane, two action rows. Opened by the home screen's small
**Mods** button (home window, row under Multiplayer|Settings, paired with About; control key `home.mods`) and by Settings > Experiments > *Mods* (a `KIND_TEXT` row `mods_open` that closes
Settings and opens the screen; in a world it only explains that mods change from the main menu). Escape = a row of `Menu::back()`
(`Classes/Menu.cpp`, QA.22). No text input anywhere on it, so the touch keyboard cannot cover anything.

- **Installed** (always): `ContentPack::scanPacks(documents, &lines)` on open and after every change, one row per pack folder (name, else id,
  else folder; right-hand word On / Off / Refused / Skipped). Row 1 buttons act on the selection: *Turn off/on* (`setPackEnabled`), *Up*/*Down*
  (`movePack`; `--packs=` rows are fixed and the detail pane says so), *Remove* (`removePackFolder` behind a `GLDialog` naming `Packs/<folder>`);
  a Refused pack has Remove only. Row 2: *Install from file...* (desktop only: `eden_pick_zip_*`, SDL's open-file dialog in `Input_native.cpp`;
  iOS/Android report it unavailable and the button is not drawn; the result goes to `PackInstall::installZip(path, docs, "")`) and the footer,
  "Changes apply when a world opens" until an action replaces it with its outcome. The detail pane: id/version, folder, state + block count,
  the loader's `why` (amber), requires/conflicts, then the validator lines (errors red, `warning:` grey).
- **Get Mods** (K.3-6): the tab rail exists only while the setting `mods_catalogue_url` (Settings > Experiments > *Get Mods catalogue*, a
  `KIND_TEXT` row; http/https only) is non-empty (K.3-8: it defaults to `https://hagg3.github.io/emod-packs/index.json`). The catalogue is `eden_net_fetch(url, NULL)` on first show / *Refresh*, parsed by
  `PackInstall::parseCatalogue`; rows say "Installed" by id and the detail pane "installed vX". *Install* downloads to
  `Packs/.download.partial` (progress bar, polled per frame, *Cancel* in place), then `installZip(partial, docs, entry.sha256)`; the partial is
  always deleted and a failure shows `error` plus the validator lines. No catalogue host exists yet.
- **World entry notice**: `World::modsNoticePending` (set where `loadMods` runs, never in a session) is consumed on the first non-wait frame;
  any `ContentPack::worldMods` entry that is not loaded makes one status toast "This world uses mods that are off or missing: a, b".
- **Multiplayer**: `EdenMpServerRow::usesMods` (INFO `mods=` present; `modsCompatible(info, "")` false) makes the server row read "Uses mods"
  and `ServerListMenu::join` refuse with the toast "This server uses mods (not supported yet)" before any connection.
- Gates: `--ui-selftest` (`mods_ui_checks` in `mode_ui.cpp`: open, select, toggle, move, remove with confirm, install from file, tab hidden /
  shown, catalogue bytes via `ModsMenu::feedCatalogue`, failed download leaves no partial); `--shot` pages `mods`, `mods-skipped`,
  `mods-refused`, `mods-get` (`TESTERS/k3/`: desktop 1280x800, iPad `--window=1024x768 --touch-profile`, phone `--window=844x390 --touch-profile`).

## Keyboard focus and hover (UX.7)

`GLW::Focus` (`Classes/GLWidgets.{h,cpp}`) is the kit's focus model. There is no per-screen list of
controls: **every kit control's `render()` registers itself** (rect + kind), and the keys act on what the
top screen drew last frame.

- **Opting in is one call** at the top of a screen's `render()`: `GLW::Focus::screen(this, &defaultButton)`.
  The last `screen()` call of a frame wins (painter's order), so a `GLDialog` over a screen, or Settings
  over the pause column, takes the focus without the screen under it knowing. Controls drawn under another
  owner are not focusable and not hovered. `keys = false` (the Console) gives hover only: its code field
  owns Tab and the arrows. Wired: home, My Worlds, About, Settings (+ Keys), New World, World Properties,
  Mods, Get Worlds, the server list, its form and Connecting window, `GLDialog`, the pause menu, the Console.
- **Keys** (from `Input_native.cpp`, only while the cursor is free and a keys screen is up; they then win
  over play bindings such as Space = jump): Tab / Shift-Tab step through the stops, sorted top to bottom
  then left to right; a list's rows, a `TabRail` and an `IconRail` are **one stop each**, entered at the
  selected member. Arrows go to the nearest control that way; inside a list Up/Down also **select** the row
  and scroll at the edge (then leave the list when there is no more to show); Left/Right on a `Slider` or
  `Stepper` change the value. **Return** activates a focused button, otherwise the screen's default button
  (Play, Join, Save, Download, Resume, a prompt's or a one-button dialog's button 0); **Space** activates
  the focused control. A two-button confirm has **no** Return default (Delete / Cancel needs a focused
  button). Escape is not Focus's: the back chain keeps it.
- **Activation is a synthetic tap** on the control's rect (`eden_input_pointer_event`, identity -24, down
  and up two frames apart), the trick the HUD keys already use, so a screen's claim protocol runs unchanged
  and there is no second action path. Scrolling is a synthetic wheel notch (`eden_ui_push_wheel`), which
  every list screen already feeds to `scrollBy()`. So a new kit screen gets keyboard navigation from
  `Focus::screen()` alone, provided it scrolls on `eden_ui_take_wheel()` and acts on taps.
- **In a text field** every key is the field's (the text seam); a non-code field hands Tab, Shift-Tab
  (`EDEN_TEXT_KEY_BACKTAB`), Up and Down to `Focus::key()`, which blurs it and moves on. Tab onto a field
  taps it, so the screen's own focus rule (keep-visible rect, keyboard kind) applies.
- **A ScrollView's `render()` adopts** the controls drawn since the previous one that sit beside its bar and
  left of it (that is how Up/Down know which list to scroll): draw a list's rows before its `ScrollView`.
  A row a screen draws itself (the server list) calls `GLW::Focus::row()` after its background.
- **The ring** is 2u of `--eden-green-700`, 2u outside the control (inside a list row), drawn by the
  control's own `render()`. It shows only after a navigation key and goes at the next click or touch.
- **Hover** is a TINT wash (lime; white on a toned button) in each control's `render()` while
  `eden_ui_pointer()` reports the mouse inside its hit box: cursor free, no button down, last input not a
  finger. Pressed, selected and disabled controls do not tint.
- **Touch shows neither.** On the touch input profile (`GLW::touchFloor() > 0`) there is no ring, no tint
  and the keys are not consumed.
- `Focus::frame()` runs once a frame from `eden_native_input_tick()` (before `World::update`): it swaps the
  collected list in, ends a pending tap, then acts on queued keys. A harness mode that does not run the
  input tick (`--shot`) therefore draws no ring and no tint, except its `menu-focus` page, which steps the
  model by hand.
- Gate: `--ui-selftest` (pointer: hover counts, Tab order on My Worlds, wrap, arrows move
  `selected_world` and scroll, dialog focus, Tab cycles, the New World field, Settings Space/Right, Return
  plays; `--touch-profile`: zero rings, zero tints, keys untouched). Headless has no pixels, so the touch
  leg asserts the kit's ring/tint draw counters (`Focus::debugCounts`) rather than a pixel.
- Not done: the `IconRail` does not scroll to a focused tile that is off its end (no screen has one on a
  pointer profile); the block/colour pickers and the HUD's own buttons are not kit screens and have no
  focus or hover.

## Dead/auxiliary UI code
- `Classes/Toolbar.cpp` and `Classes/Gamepad.cpp` — compiled, but each file's body is one `/* … */`
  comment (stock archaeology), so they contribute nothing. Port gamepad support is SDL's
  (`SDL_Gamepad` in `port/src/seam/Input_native.cpp`).
- `settings.xib`, `prototypeViewController.xib` — legacy nibs, not part of the GL UI.

## Common pitfalls
- Layout constants are absolute points written for specific devices; the port derives the point
  space at run time (the MODIFIED FROM STOCK note above), so test with `--shot`, `--display-selftest`
  and `--touch-selftest --touch-profile` rather than per device (`IS_IPAD` still means "2× UI
  scale", see [conventions-and-pitfalls.md](conventions-and-pitfalls.md)).
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

**Where it lives:** `port/src/seam/Settings.cpp`, in its own `KEYBINDS` section; `KeybindsMenu`
(the Keys tab) is its one front end since the web build (and its DOM Keys tab) retired. Read that section's header comment before
changing anything here; the summary:

- **A binding is an integer: the key's USB HID usage ID.** SDL3's `SDL_Scancode` values *are* those
  IDs (the retired browser's `event.code` strings were a 1:1 renaming of the same table). An int below 512 is exactly representable in a float and persists as an `NSNumber`.
  `port/src/seam/Input_native.cpp` carries `static_assert`s on the scancode/HID identity, so an
  SDL that renumbered would break the build rather than silently rebind every keyboard.
- **`kKeyNames[]`** is the one place the key's spelling, the HID number and the display label are
  related. (`eden_keybind_code_table()` still emits it as JSON — the retired web page's reader.)
- **`kKeybinds[]`** is the action table: id, label, group, default primary, a fixed secondary, the
  continuous/momentary dispatch class, and which hosts implement it (`settings`
  is `KB_WEB`-only, so no live target has it; `fullscreen` went native in UX.5; `eden_keybind_native_hidden()` is how the GL screen skips them — the same
  policy `eden_settings_native_hidden()` applies to settings rows).
- **These are NOT `kSettings[]` rows.** The stage plan called for a `KIND_KEY` row type inside
  `Setting`; a keybind carries four fields a float row has no place for, and `min/max/step/def`
  would all be dead weight. `KIND_KEY` exists as a kind constant for row dispatch, not as a member
  of `kSettings[]`. `eden_settings_reset_all()` resets both tables.
- **Conflicts are allowed, deliberately.** Ctrl ships bound to *both* `flyDown` and `crouch`, so a
  "clear the other binding" rule would fight the shipped configuration on the first rebind. The
  lookup is therefore an iterator (`eden_keybind_action_after(prev, code)`), and every caller must
  walk it rather than stop at the first hit.
- **Gate:** `./build/eden_native --keybind-selftest` (manual; not in CI) asserts both directions of a rebind — that the
  new key works *and* that the old one has stopped — because a test of only the new key would pass
  against an input path that still had its hard-coded `switch (sc)` and merely also consulted the
  model. It measures "stopped" against an unbound control key rather than a fixed epsilon, since a
  flying player drifts and the drift is a property of the world, not of the binding.

## Fly buttons (FM.3)
Bottom-right row, right to left: **jump**, **crouch**, **fly-cycle**, 10 pt apart (`flyGap` in
`Hud::layoutForScreen`; `inbox2/3` add 4 pt slop per side, so a gap under 8 made hit rects overlap).
Hit rects equal the render rects (jump's still runs on to the screen edge). While `FLY_MODE`
(Fly or Noclip) the jump slot is **Fly up** and the crouch slot **Fly down** (shown even with
Crouch off): the touch sets the frame-derived `hud->m_flyup/m_flydown` instead of `m_jump/m_crouch`
and `Player` ORs them with the keyboard `FLY_UP/FLY_DOWN`. The old tap-latch on the fire/pickaxe
buttons is gone: they no longer move y. The cycle button (`rflyhit/rflyrender`) exists only when
*Allow fly* is on and the menu is closed; a tap (claim on down, act on release, like `rchat`) calls
`fly_cycle_state()`. Its glyph is a GL-kit `Label` ("OFF" / "FLY" / "NC", lime while flying) on a
dark plate, built once per state; no new art.

## Version tag
`GLW::drawVersionTag()` (GLWidgets.cpp) draws "Emod <version>" (white, 1u black shadow) in the top-left
corner: from `Menu::render` after `renderKit()`/`renderHome()` (home and the world list; not the server screens, whose Back sits there: UX.3) and at the end of
`Hud::render` (hidden with the UI and in camera mode), where `drawVersionTag(xOffset)` moves it right of the corner buttons (menu gear, then chat/console). The touch jump button is not drawn when `!eden_profile_touch_chrome()` (its hit rect stays: Space drives it). The string is `EMOD_VERSION_STRING`, defined by
`native/CMakeLists.txt` from the repo-root `VERSION.txt` (fallback "dev"); the iOS Info.plist takes the
same value. Change it with `./bump-version.sh`. (Not `VERSION`: on case-insensitive filesystems
`#include <version>` finds it.)

## Hide-HUD, screenshot and build-size keys (H.5, H.7)
Three native-only `kKeybinds[]` rows (`Settings.cpp`): **F1** `hideHud` flips `hud->hideui`
(`Hud::render` already skips the HUD when set), **F2** `screenshot` sets `MODE_CAMERA` + `take_screenshot`
so `Hud::render`'s camera branch takes the shot, flashes, plays the shutter and returns to build mode
(one path with the on-screen camera button), **X** `buildSize` toggles `hud->build_size` 1↔2 (never 0, the
retired half-size brush). X does nothing unless Experiments › *Double-size build brush* (`double_brush`,
`DOUBLE_BRUSH` in `Player.cpp`, default off) is on; turning it off resets the size to 1. While on, the build
button shows the `blocktoggle` icon (`ICO_SIZETOGGLE2` small→big, `ICO_SIZETOGGLE1` big→small) as a badge.
`buildBrush2` (`Player.cpp`) places a 2×2×2 block at the aimed cell, filling only air or shallow liquid cells,
skipping the whole brush if it would pass the world top, spending one golden cube at most, with one sound per
tap; the player-box test (`Player::test(...,2)`) was already in place.

**Atmosphere settings (Stage AT.1a).** Gameplay › *Weather* (`weather`, default off; `eden_weather_enabled()`), Video › *Atmosphere* Off/Low/High/Custom
(`atmosphere`; default High, seeded Off once on Air 2-class iOS; a fourth knob of the Graphics preset: Low Off, Default the device default, High High)
and Video › *Reduce flashing* (`reduce_flashing`). A tier change writes nine element rows (group `Atmo`, drawn on the Video › *Atmosphere details* page (SX.5, `kPages[]` id `atmosphere`), cvars `atmo.precip`, `splash`, `clouds`,
`cloud_shadows`, `height_fog`, `ambient`, `water_tint`, `glint`, `halos`) from `kAtmoTier[]` in `Settings.cpp`; the renderer reads `eden_atmo_element(AT_*)` (the `AT_*` enum is in `Classes/Weather.h`, QA.25; insert new elements before `AT_COUNT`),
which is 0 whenever the tier is Off (callers do not re-test `eden_atmo_tier()>0`; the High-only `>=2` tests stay, because a Low tier can carry a hand-set High-only row). SX.5: editing an element row so the nine match no tier sets the tier to *Custom* (3; `eden_atmo_tier()` clamps it to 2 so Custom gates like High and the rows decide); restoring a tier's rows snaps it back (`eden_atmo_sync`). Custom drops the Graphics preset to Custom. `--atmo=off|low|high` and `--weather=off|clear|overcast|rain|snow|storm` override for one run (not stored);
High leaves cloud shadows off (opt-in). `--stage1`, `--scene-shot` and `--sky-shot` force both off unless given. Gate: `--weather-selftest=settings`.

**Day/night cycle (Stage DN).** Experiments › *Day/night cycle* (`day_night_cycle`, default off) and *Day length*
(`day_length`, 10/20/40/60 min, default 20; `Settings.cpp`). While on, the sky brush shows a status-bar toast
(*"The day/night cycle is on: it sets the sky…"*, `Player.cpp`) and paints nothing. World Properties › World has
*Lock time of day* (Emod worlds; `WorldProps.skyLocked`/`skyT`, written as `SKY_CLOCK` in the Save batch; the live
clock if that world is open). AT.1c adds *Lock weather* the same way (`WorldProps.weatherLocked`, `WEATHER` flag bit 0, `WP_WROTE_WEATHER`). Look and clock: `docs/lighting-liquids-effects.md`.

**Camera view (H.12).** Experiments › *Camera view* (`camera_view`, enum First person / Third person /
Third person (front), default first) and a native-only `kKeybinds[]` row **F5** `cameraView` (Interface)
that cycles it. The key goes **through** the setting (`eden_cycle_camera_view`, `Settings.cpp`), so the
live `CAMERA_VIEW`, the Settings row and the persisted value never disagree; touch devices use the row. No
gamepad binding. Engine side: [player-input-camera.md](player-input-camera.md) § Camera.

### Q.1a notes (2026-10-21)
- **F5 (menu background):** the four undrawn cloud/mountain menu textures (`cloud_SM`, `mountains`, `cloud_MD`, `cloud_LG`) are no longer decoded; their `menutextures` slots hold `NULL` (positional). **F25 (Console screen):** `ConsoleMenu` keeps one rasterised-line cache per source tab (`m_cache` / `m_cacheOther`, swapped on a tab switch) instead of rebuilding.

## Strings, tones and hint colour (UX.4)
- **Sentence case** for every label and title except proper names (`My Worlds`, `Get Worlds`, `Get Mods`, `About Emod`): `port/tools/ui-string-gate.py` fails a Title Case literal on a `setLabel`/`.set`/`setTitle` line or one marked `// ui-label` (the pause-menu and dialog button arrays); names go in its `NAMES`.
- **Danger tone** on destructive choices: My Worlds *Delete* (`k->del`), the delete dialog and *Change terrain* (`GLDialog::setDanger(j)`, called right after `show()`; reset with the dialog).
- **Colours:** `kTextHint` #6b6b6b (5.3:1) for list dates and "not running" sub-lines; `kTextMuted` #979797 only for rows meant to look inert (muted, disabled).
- Server list: the profile button reads "Profile: <name>" with the `USERS` icon, or "Set a name first"; Join stays disabled until a name exists. My Worlds no longer sets the "Choose world to load" status.

## Desktop window basics (UX.5)
`port/src/seam/WindowState_native.cpp` (no-ops on iOS): minimum window 568x320 (no window aspect floor: `SDL_SetWindowAspectRatio` with min != max makes AppKit trap when leaving fullscreen on macOS with SDL 3.4.12, so a window narrower than 1.2:1 is allowed and `DisplayProfile` letterboxes it), the windowed rectangle saved at quit as prefs `window.x/y/w/h` (an interactive run only, and a size that fails `eden_window_size_ok` is ignored), and the Settings > Video *Fullscreen* toggle (`fullscreen`, hidden on iOS) that F11 (`fullscreen` keybind, native) flips; the OS's own fullscreen changes write back to the setting. The home screen's **Quit** row shows only for the desktop input profile (`eden_home_quit_offered`), so `--device=iphone16` has none.
