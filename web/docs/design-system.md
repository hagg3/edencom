# Eden: Community Edition — Design System

The single visual language for every DOM surface in the web port: the main menu, Load World, New
World, settings, the pause menu, the load-failure dialog, the boot screen and the in-game chrome —
**and, since Phase N Stage 5.1, for the engine's own GL screens on every native target too**. See
"The GL twin" near the bottom: same tokens, same three mechanics, different drawing calls.

**Source of truth is the code**, in three files:

| File | What it is |
|---|---|
| [`../public/eden-ui.css`](../public/eden-ui.css) | Tokens + every component. The whole visual system. |
| [`../public/eden-ui.js`](../public/eden-ui.js) | `window.EdenUI` — factories that build the components, the `--u` scale unit, scrollbar wiring, focus trapping. |
| [`../public/eden-icons.js`](../public/eden-icons.js) | `window.EdenIcons` — the inline SVG icon set. |

This document explains the *why*. When it and the CSS disagree, the CSS is right.

## Provenance

Derived from the Figma file `EMOD.fig` (four example screens: Main Menu, Gameplay Settings, New
World, Load World) by way of the Claude Design project **"Eden: Community Edition Design System"**
(`3ae5f207-f42d-4fc4-bcc6-fba46732b8e0`), which extracted the real token values.

**Where the kit and the mockups disagreed, the mockups won.** The deviations are listed at the
bottom — read them before assuming the kit is authoritative for anything.

## The look, in one paragraph

Chunky, tactile, hardware-rendered "arcade cabinet" skeuomorphism. Thick light-gray beveled
controls that look pressable, **hard-edged drop shadows** (zero blur anywhere in the system), dense
inset keylines, **square corners** (radius 0, no exceptions), and a pixel display face (Jersey 10)
for everything the player reads as chrome. Color is reserved for exactly two jobs — the lime
"on"/positive state, and the painted voxel background art. No soft shadows, no `backdrop-filter`,
no rounded corners, no eased transitions: every control snaps between DEFAULT / HOVER / PRESS.

## The three mechanics

Everything is built from these. **Do not invent a fourth.**

| | Recipe | Used for |
|---|---|---|
| **RAISED** | dark inset keyline + hard offset drop shadow + inset highlight on the far corner (fake top-left light source) | anything you press: buttons, tab tiles, the scrollbar thumb, the slider thumb |
| **SUNKEN** | the same keyline + an inset shadow pointing INWARD, no drop shadow | anything you set a value in: text field, scroll track, toggle track, slider track, progress track |
| **PRESSED** | gradient removed for a flat mid-gray fill, keyline becomes a plain solid border, deep inset shadow, label recolors light with a **black** text-shadow | held down **and** selected — in this system those are the same visual |

Tokens: `--bevel-raised`, `--bevel-sunken`, `--bevel-pressed`, plus `--bevel-window` /
`--bevel-content` for the two surfaces.

That "pressed == selected" equivalence is load-bearing. It is why the segmented control, the tab
rail and the New World height-format picker need no components of their own — they are ordinary
buttons with `aria-pressed` / `aria-selected` / `.is-active`.

### The 6% gradient

Button faces are `linear-gradient(180deg, white 6%, #c9c9c9 6%)`. That is **not a blend** — it is a
hard one-stop fake top-edge highlight, and it is the single most recognizable thing about this
button. Don't smooth it.

## Scale: the `--u` unit

Every dimension in `eden-ui.css` is authored in mockup pixels against the Figma frame (783×587) and
multiplied by `--u`:

```css
width: calc(160 * var(--u));   /* the 160px home tile */
```

`eden-ui.js` recomputes `--u` from the viewport (`min(vw/783, vh/587)`, clamped to 1–2.4) on load,
resize and orientation change. A fixed-pixel UI authored at iPad size would be a postage stamp on a
desktop monitor; this keeps the chunk proportional at any size, hit targets included.

**Write new rules as `calc(N * var(--u))`, never bare px**, or they will not scale with the rest of
the system. The two deliberate exceptions are documented in place: the crosshair (sized against the
*scene*, not the UI) and the boot status line (a debug readout).

## Layout contract for full screens

Settings, Load World and New World are **fluid, not fixed-canvas**. The window fills the viewport in
both axes less `--eden-gutter`, up to a generous `max-width`, and everything inside scrolls:

- `.eden-scrim` / `.eden-menu__center` — `display:flex; align-items:stretch; padding:var(--eden-gutter)`
- `.eden-window` — `width:100%; height:100%; max-width:calc(1100 * var(--u))`
- `.eden-content`, `.eden-tabrail` — `overflow-y:auto`, so a short viewport compresses rather than clips

Reproducing the mockup's fixed 783×587 canvas literally would have left a small panel marooned in
the middle of a desktop window and a cramped one on a phone.

**Dialogs opt out of both axes** (`.eden-window--dialog`: `align-self:center; height:auto`) — a
six-button pause menu stretched to a 1400px-tall panel is absurd.

## Components

| Class | Kit name | Notes |
|---|---|---|
| `.eden-btn` + `--sm/--square/--tab/--md/--lg` | `Btn-Small`, `Btn-Square`, `btnTab`, `btnHomeMedium`, `btnHomeLarge` | One engine, six size presets. The kit shipped these as six separate components with no visual difference beyond size. |
| `.eden-btn--positive` / `--danger` | (positive only) | Danger is a system addition — the mockups have no destructive state, the port does. |
| `.eden-btn.is-placeholder` | — | System addition. See "Placeholders and disabled controls" below. |
| `.eden-btn:disabled` | — | Genuinely inert. `.eden-btn--tab:disabled` adds grayscale, since a rail tab sits on opaque chrome rather than over the background art. |
| `.eden-window` + `--narrow/--dialog/--fit` | `Window-Standard` | 90%-opaque light gray, 3px dark inset border, hard 4px drop shadow. `--fit` shrink-wraps to content (the pause menu). |
| `.eden-titlebar` | `WindowHeader` | Back at left, title **absolutely centered on the window**, actions at right. |
| `.eden-content` | `ContentBox-List` | Flat opaque white, 1px hairline, no shadow — the calmest surface in the system. |
| `.eden-tabrail` | `SettingsTabRow` | Vertical icon rail. |
| `.eden-scrollbar` | `scrollBar` | Wired for real (kit shipped it decorative). |
| `.eden-listrow` | `ListEntry` | Pixel title, sans description, actions at right. |
| `.eden-toggle` | `TOGGLES-RecreateAsVectors` | Split-pill, no sliding knob. |
| `.eden-field` | `textField` | |
| `.eden-radio` | `radioButton` | |
| `.eden-seg` | — | Just a flex row of buttons; see "pressed == selected". |
| `.eden-slider`, `.eden-progress`, `.eden-stack` (+ `--left`, `--grid`) | — | System additions; built from the same two mechanics so they don't read as foreign. `--left` left-aligns icon+label, turning the stack from a centred choice into a scannable menu; `--grid` puts it in two columns (one below 420px). |
| `.eden-hotbar`, `.eden-toast`, `.eden-statusline` | — | In-game chrome. See below. |

### Type

Two faces, in an unusual split: **Jersey 10** (self-hosted pixel display face) does *everything the
player reads as chrome* — titles, button labels, list titles, even the text field's placeholder.
The **body sans** appears only in actual descriptive sentences (settings descriptions, world
metadata). The "body" face is used far less than the "display" face.

Raised chrome text is black with a white 1px offset shadow; pressed chrome inverts **both**. That
inversion is the press cue.

### Iconography

The Figma types its icons as literal **Apple SF Symbols** private-use characters in "SF Pro" — an
iOS-only, closed-license dependency that cannot ship on the web. They are replaced with the open,
ISC-licensed **Lucide** visual language, inlined as SVG path data in `eden-icons.js`. No CDN fetch,
no sprite sheet, no CORS story. Icons inherit `currentColor`, which is what makes them invert for
free with the button's press state.

Default stroke weight is **2.4**, not Lucide's 2 — at 2 they read as spindly next to this UI's
2–3px keylines.

**Raster art is the deliberate exception**, and every instance of it is the *engine's own* art
loaded by [`../public/eden-assets.js`](../public/eden-assets.js) — never a new PNG authored for the
web UI. Two groups:

- **The title screen**: three home-tile icons, the "EDEN" wordmark, the parallax layers
  (`media/ipad_menu/`). Redrawing these as SVG would lose the thing that makes the title screen look
  like Eden.
- **Action icons** (`media/ui/`): the pause menu's Save / Warp Home / Take Photo / Quit and the
  Settings button on both menus, via `EdenUI.button({ iconImg: … })`. These are the same little
  painted objects the 2010 HUD drew for those exact actions, and using them keeps the DOM menu
  reading as the same game rather than as a generic web panel.

The trade-off to know: an `<img>` cannot inherit `currentColor`, so a raster icon does **not** invert
with the button's press state the way a Lucide glyph does. That is fine for full-colour objects and
wrong for anything meant to read as a glyph — which is why Resume keeps the vector `play` triangle.

### In-game chrome

The hotbar, toast and status line sit **over the rendered world** rather than on a panel, so they
invert the palette: dark surfaces, light text. Same bevel mechanics, flipped colors — recognizably
part of the same UI without fighting the scene for attention.

### Placeholders and disabled controls

The mockups specify controls the port cannot do yet: **Get Worlds** (needs the edengame.net client),
per-world **Share** and **Info**, the **Biome** generator, and the **New Dawn 256z** height format
(blocked by the frozen `T_HEIGHT` — see the root `CLAUDE.md`'s format freeze).

There are two ways to render one, and the choice is about whether there is anything to *say*:

- **`placeholder: true`** — "this will exist, here's why it doesn't yet". Stays focusable so the
  explanation is reachable. Use it when the absence is worth explaining.
- **`disabled: true`** — a real `disabled` attribute: no hover, no press, no click, out of the tab
  order, greyed. Use it when the control should simply read as switched off and there is no story to
  tell. The New World screen's third generator tab is the case that motivated it — a rail tab that
  lit up under the cursor and explained itself in a tooltip was noisier than one that is just off.

`EdenUI.button({ placeholder: true, placeholderNote: '…' })` gives:

- `aria-disabled="true"` and a click swallower, **but not `disabled`** — a `disabled` button is
  skipped by Tab and announces nothing, so a screen-reader user would never learn the control exists
- a visually-hidden note explaining *why* it does nothing, and the same text as a tooltip
- muted contents (icon/art at 40% and grayscaled) but **solid chrome** — the `:disabled` 0.55
  opacity turns the whole control into a translucent ghost you can see the background art through,
  which reads as a rendering bug rather than as a state

## Assets

`eden-assets.js` reads the menu art **back out of the Emscripten virtual filesystem** and wraps it
in `blob:` URLs, cached by path. The PNGs are already shipped — CMakeLists `--preload-file`s
`media/ipad_menu` into `/bundle/media/ipad_menu` for `Menu_background.mm` to draw. Copying them into
`public/assets/` would have shipped ~2 MB of art twice. This way there is one copy, and the DOM menu
is *guaranteed* to be showing the same art as the GL menu it replaces.

The one genuinely new binary is the font: `public/assets/fonts/Jersey10-Regular.ttf` (SIL OFL, see
`OFL.txt` beside it), self-hosted so the game works offline and from a plain static server.

## Accessibility

Not a coat of paint on top — these are the reasons several components are shaped the way they are:

- **The toggle is a real `<input type="checkbox">** at full size and zero opacity over two painted
  halves. That is what keeps Space/Enter, `:checked`, `:disabled`, focus and screen-reader semantics
  working with no JS at all. The two `<span>`s are pure decoration.
- **`EdenUI.railKeyNav`** applies the ARIA tabs pattern to the vertical rail: roving tabindex,
  Up/Down between tabs, Home/End to the ends. Without it a 7-tab rail costs 7 Tab presses to walk
  past, and arrow keys — what a screen-reader user will actually try on a `tablist` — do nothing.
- **`EdenUI.trapFocus`** contains Tab inside modal surfaces and restores focus on release. The panels
  this system replaced were modal in appearance only; Tab walked straight out of them into the page
  behind. Every call site must release it in its close path — a leaked trap captures the page's Tab
  key forever on a detached node.
- **The native scrollbar is hidden, not disabled.** `.eden-content` stays genuinely scrollable, so
  wheel, touch, PageUp/Down, arrows and find-in-page all still work; the chunky scrollbar is a
  visual plus a drag handle on top.
- **Focus rings** are the one place a color outside the two sanctioned jobs is used. That is an
  accessibility requirement, not decoration.
- **`@media (forced-colors: active)`** puts real borders back: the entire system is expressed in
  `box-shadow`, which forced-colors mode discards, leaving invisible edgeless controls.
- **Touch floor.** `--u` bottoms out at 1, where a 29u button would be a 29px tap target. A
  `pointer: coarse` block raises the *hit box* (not the art) to the 44px platform minimum.
- **Live regions** on the things that would otherwise change silently: the toast (the only feedback
  the fly-mode/block-preview shortcuts give), the load-failure message, load progress.

## Motion

There is none, by design — the source snaps between three discrete states. `--eden-duration` is
`0ms`. The exceptions are all decoration or progress, and all stop under
`prefers-reduced-motion: reduce`: the menu's parallax drift and pinwheel rotation (matched to
`Menu_background::update`'s own rates), the progress-bar fill, and the boot screen's indeterminate
sweep.

## Deviations from the Claude Design kit

The kit is an honest extraction, but a few things in it are wrong for a real build:

1. **No Rubik.** The kit `@import`s it from Google Fonts. The port self-hosts Jersey 10 and uses the
   platform sans stack for body text — no external network request, so the game keeps working
   offline and from `file://`.
2. **Fluid windows, not a fixed 783×587 canvas.** See "Layout contract" above.
3. **The scrollbar is functional**, not the decorative element the kit shipped.
4. **Two icon paths were mangled in the kit** and rendered as meaningless shapes — `wrench` (a bare
   diagonal bar, no head) and `mountain` (two chevrons, no peak). Both replaced with Lucide's actual
   geometry. If you re-import from the kit, re-check these.
5. **Toggle geometry** is 104×34u, not the kit's 110×40 — measured off the Settings mockup.
6. **Added:** `danger` tone, slider, progress bar, stack, placeholder state, in-game chrome, and a
   selection tint for list rows. None have a mockup counterpart; all are built from the existing
   mechanics rather than introducing new ones.
7. **Selection is a tint, not a fill.** The mockups show no selected state; a saturated lime bar
   across a white row fights the pixel type sitting on it.

## The GL twin: `Classes/GLWidgets.{h,mm}`

**This document is no longer only about DOM surfaces.** Phase N Stage 5.1 transcribed the system
into the engine's own GL drawing (`GLW::` in `Classes/GLWidgets.h`) so the native desktop builds,
iOS/Android and the web build's `legacy_menu` fallback all render the same language with no
toolkit and no new dependency. It was affordable for one reason: **every bevel here is a stack of
hard-edged rectangles**, so the GL twin needs no atlas art and no shader — `Graphics::drawRect`
fills, in the same order the CSS `box-shadow` list would composite.

What carries over exactly: the palette, the three mechanics plus the two surfaces
(`BEVEL_RAISED`/`SUNKEN`/`PRESSED`/`WINDOW`/`CONTENT`), the 6% gradient, square corners, the 1u
chrome text shadow and its press inversion, the 44pt touch floor.

What differs, and why:

| | DOM | GL kit |
|---|---|---|
| Scale unit | `--u` = `min(vw/783, vh/587)` clamped 1–2.4 | `GLW::u()`, same formula but against **point space** (~1138×640), clamped 0.7–2.4 — the display profile has already normalised the denominator, so the useful range is narrower |
| Type | Jersey 10 for chrome, platform sans for body | **one** platform font at two sizes. The raster seam (`eden_rasterize_text_rgba`) takes a size and an alignment, not a family; giving native the pixel display face means giving that seam a font argument first |
| Y axis | down | **up.** Every `--eden-drop-*` is drawn at −y and every `inset a a` is a strip on the TOP and LEFT edges |
| Wrapping | the browser measures | estimated at `0.52 × pt` per character — there is no measure call in the seam. Costs at worst one extra break in a body sentence |
| Icons | Lucide SVG, `currentColor` | **not yet.** The mockups' alert glyph is owed; it needs either vector path playback or an engine atlas tile |

### Alert-2Stack

The dialog pattern the mockups specify (`Alert-2Stack`), and what `GLDialog` renders:

```
+--------------------------------------------------+   WINDOW bevel, 420u wide
|  Choose world type                        [icon] |   title, display size, LEFT aligned
|  Normal is the generated landscape. Flat is...   |   body, secondary colour, wrapped
|  +---------------------+  +---------------------+ |
|  |        Flat         |  |       Normal        | |   RAISED buttons, two columns
|  +---------------------+  +---------------------+ |
|  +----------------------------------------------+ |
|  |                   Cancel                     | |   an ODD FINAL button spans both
|  +----------------------------------------------+ |
+--------------------------------------------------+
```

Two rules fall out of it, and both are load-bearing for callers:

1. **Buttons are a two-column grid, and an odd final button spans the full width.** So "Cancel goes
   last" is not a copy convention, it is how Cancel gets its own row. A seam that passes Cancel
   first (as `showAlertWarpHome` did until Stage 5.1) gets it in the top-left cell instead.
2. **The title is left-aligned with a body sentence under it**, not a centred one-liner. A dialog
   with nothing to explain passes `NULL` for the body and the grid moves up.

## Adding to the system

- New dimension? `calc(N * var(--u))`.
- New surface? Compose `.eden-window` + `.eden-content` + existing controls. If you think you need a
  new bevel, you almost certainly need RAISED, SUNKEN or PRESSED.
- New control that holds a value? It is SUNKEN.
- New icon? Add the Lucide path to `eden-icons.js`. Never a CDN, and never a *new* PNG — the only
  raster allowed is art the engine already ships, reached through `eden-assets.js`
  (`button({ iconImg: … })`); see "Iconography".
- New setting row? Add it to `kSettings[]` in `src/seam/Settings_web.mm` — **never** to the JS. The
  panel renders the C schema generically.
