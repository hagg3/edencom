# Eden: Community Edition — Design System

The single visual language for the engine's GL screens on every native target — main menu, World
list, New World, Settings, the pause menu, dialogs, the world browser and in-game chrome. It was
first built as CSS for the web build's DOM screens (retired 2026-10-13); since Phase N Stage 5.1 the
live implementation is the GL kit, **`Classes/GLWidgets.{h,mm}`** ("The GL twin" below: same tokens,
same three mechanics, different drawing calls).

**Source of truth is the code.** The CSS it was written against is at the tag
(`git show web-port-final:web/public/eden-ui.css`, with `eden-ui.js`); the icon set is still live as
[`../tools/eden-icons.js`](../tools/eden-icons.js) (Lucide SVG paths, rasterised by
`port/tools/ui-icons.py` into `media/ui/ui_icons.png`). CSS class names (`.eden-btn`, `.eden-tabrail`…)
are kept below as the design vocabulary the GL code comments use.

This document explains the *why*. When it and `GLWidgets` disagree, the code is right.

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
`--bevel-content` for the two surfaces (GL: `BEVEL_RAISED`/`SUNKEN`/`PRESSED`/`WINDOW`/`CONTENT`).

That "pressed == selected" equivalence is load-bearing. It is why the segmented control, the tab
rail and the New World height-format picker need no components of their own — they are ordinary
buttons drawn PRESSED when selected.

### The 6% gradient

Button faces are `linear-gradient(180deg, white 6%, #c9c9c9 6%)`. That is **not a blend** — it is a
hard one-stop fake top-edge highlight, and it is the single most recognizable thing about this
button. Don't smooth it.

## Scale: the `u` unit

Every dimension is authored in mockup pixels against the Figma frame (783×587) and multiplied by
the scale unit — CSS `calc(N * var(--u))`, GL `du(N)` / `N * GLW::u()`. `u = min(W/783, H/587)`; the
CSS took W,H in viewport pixels (clamped 1–2.4), the GL kit takes them in **point space** (clamped
0.7–2.4; see the GL twin table). **Write new layout as `du(N)`, never bare points**, or it will not
scale with the rest of the system. The GL Settings and New World screens use one **fixed panel**
(the 783×587u frame less a 12u margin, `GLW::railFrame()`); dialogs shrink-wrap.

## Type and icons

### Type

Two faces, in an unusual split: **Jersey 10** (the bundled pixel display face) does *everything the
player reads as chrome* — titles, button labels, list titles, even the text field's placeholder.
The **body sans** appears only in actual descriptive sentences (settings descriptions, world
metadata). The "body" face is used far less than the "display" face.

Raised chrome text is black with a white 1px offset shadow; pressed chrome inverts **both**. That
inversion is the press cue.

### Iconography

The Figma types its icons as literal **Apple SF Symbols** private-use characters in "SF Pro" — an
iOS-only, closed-license dependency. They are replaced with the open, ISC-licensed **Lucide**
visual language (`eden-icons.js` → `media/ui/ui_icons.png`). Icons are white art tinted at draw
time, which is what makes them invert with the button's press state (CSS: `currentColor`).

Default stroke weight is **2.4**, not Lucide's 2 — at 2 they read as spindly next to this UI's
2–3px keylines.

**Raster art is the deliberate exception**, and every instance of it is the *engine's own* art,
never a new PNG: the title screen (home-tile icons, the "EDEN" wordmark, the parallax layers in
`media/ipad_menu/`) and the action icons (`media/ui/`: Save / Warp Home / Take Photo / Quit /
Settings — the 2010 HUD's own art for those actions). A raster icon does **not** invert with the
press state, which is fine for full-colour objects and wrong for glyphs.

### In-game chrome

The hotbar, toast and status line sit **over the rendered world** rather than on a panel, so they
invert the palette: dark surfaces, light text. Same bevel mechanics, flipped colors — recognizably
part of the same UI without fighting the scene for attention.

### Disabled controls

`Button::setEnabled(false)` (CSS `:disabled`): **solid chrome**, grey label, no hover, no press, no
hit. Not a translucent ghost — the CSS's 0.55 opacity let the background art show through and read
as a rendering bug. Muted art inside a disabled tile (`IconRail`) is the same rule. (The web build
also had a focusable "placeholder" state for controls it could not offer yet; every one of those
controls — Get Worlds, Share, New Dawn 256z — now exists.)

## Accessibility (web-era)

The DOM build carried real ARIA semantics, roving-tabindex rails, focus traps, forced-colors borders
and live regions; they are at the tag. What survives in GL is the **touch floor** (44pt hit boxes on
the touch profile, art unchanged) and keyboard paths for every screen (Escape = Back, Return =
the default action, the picker's `[` `]`).

## Motion

There is none, by design — controls snap between three discrete states. The exceptions are
decoration or progress: the menu's parallax drift (`Menu_background::update`), the progress-bar
fill, and the status toast's 0.2 s fade.

## Deviations from the Claude Design kit

The kit is an honest extraction, but a few things in it are wrong for a real build:

1. **Bundled fonts.** The kit `@import`s Rubik from Google Fonts; the port bundles Jersey 10 and
   Rubik Regular (`media/fonts/`, SIL OFL) — no network request.
2. **Window size.** The web build made full screens fluid; the GL kit went back to the fixed
   783×587u frame (U.3), which the point space already scales.
3. **The scrollbar is functional**, not the decorative element the kit shipped.
4. **Two icon paths were mangled in the kit** and rendered as meaningless shapes — `wrench` (a bare
   diagonal bar, no head) and `mountain` (two chevrons, no peak). Both replaced with Lucide's actual
   geometry. If you re-import from the kit, re-check these.
5. **Toggle geometry** is 104×34u, not the kit's 110×40 — measured off the Settings mockup.
6. **Added:** `danger` tone, slider, progress bar, stack, in-game chrome, and a
   selection tint for list rows. None have a mockup counterpart; all are built from the existing
   mechanics rather than introducing new ones.
7. **Selection is a tint, not a fill.** The mockups show no selected state; a saturated lime bar
   across a white row fights the pixel type sitting on it.

## The GL twin: `Classes/GLWidgets.{h,mm}`

Phase N Stage 5.1 transcribed the system into the engine's own GL drawing (`GLW::` in
`Classes/GLWidgets.h`), so every native target renders the same language with no toolkit and no
new dependency. It was affordable for one reason: **every bevel here is a stack of
hard-edged rectangles**, so the GL twin needs no atlas art and no shader — `Graphics::drawRect`
fills, in the same order the CSS `box-shadow` list would composite. (The one atlas, since U.1, is
for the icons: `media/ui/ui_icons.png`, see "Icons" below.)

What carries over exactly: the palette, the three mechanics plus the two surfaces
(`BEVEL_RAISED`/`SUNKEN`/`PRESSED`/`WINDOW`/`CONTENT`), the 6% gradient, square corners, the 1u
chrome text shadow and its press inversion, the 44pt touch floor (on hit boxes, touch profile only).

What differs, and why:

| | CSS (web, at the tag) | GL kit |
|---|---|---|
| Scale unit | `--u` = `min(vw/783, vh/587)` clamped 1–2.4 | `GLW::u()`, same formula but against **point space** (~1138×640), clamped 0.7–2.4 — the display profile has already normalised the denominator, so the useful range is narrower |
| Type | Jersey 10 for chrome, Rubik Regular for body | **the same split** since 2026-10-05: `GLW::Label`'s `FACE_DISPLAY` (default) / `FACE_BODY`, through the raster seam's `eden_text_raster_set_face()`. Native rasterises the bundled `Jersey10-Regular.ttf` and `Rubik-Regular.ttf` (system sans only if missing) with stb_truetype. Jersey's caps are 0.50 of its pixel height against Arial's 0.64, so GL sizes are ~1.3x the body size for the same weight — the CSS's 22u buttons beside 15u body |
| Density | `pointer: coarse` raises the hit box to 44px | `GLW::touchFloor()` (44pt on the touch profile, else 0) and `setHitRect()` on every control: rows are ~30u under a mouse and 44pt under a finger, the art stays compact either way |
| Y axis | down | **up.** Every `--eden-drop-*` is drawn at −y and every `inset a a` is a strip on the TOP and LEFT edges |
| Wrapping | the browser measures | line breaks are estimated at `0.52 × pt` (body) / `0.42 × pt` (display) per character; `Label::width()` is measured by the raster seam (`eden_text_raster_measure`). Costs at worst one extra break in a body sentence |
| Icons | Lucide SVG, `currentColor` | `GLW::icon(id, rect, colour)` (U.1): the same Lucide paths rasterised offline by `port/tools/ui-icons.py` into `media/ui/ui_icons.png` (white, 64px cells; ids in `Classes/UIIcons.h`) and tinted at draw time, which is the `currentColor` inversion — `kText` on a raised face, `kTextPressed` on a pressed one. `Button::setIcon()` puts one before a label (16u) or alone in a tile |
| Controls | `.eden-toggle`, `.eden-slider`, `.eden-field`, steppers as button rows | `GLW::Toggle` / `Slider` / `Stepper` / `TextField` (Stage 5.4 / N.4.5), same geometry tokens (104×34u toggle via `Toggle::designW()/designH()`, 10u track + 16×24u thumb on `--bevel-raised-sm`, SUNKEN white field). Hit boxes sit at the 44pt floor with the art centred in them. **The toggle deliberately departs from the CSS since U.1**, toward the reference screenshots: a 2u gray-800 keyline and a gray-500 off half (the CSS's 1u keyline and gray-300 half read faint beside a 32u title), plus a muted state — lighter and opaque, where the CSS's `.is-muted` uses `opacity: .45` |
| Rows with hints | `.eden-listrow` + `__title` / hint, `.is-muted` | `GLW::TwoLineRow` (U.1): 32u display title (a 0.85 em box) over a wrapping 15u body hint, 4u/5u padding, hairline, indent, muted palette. Reports its height from the built labels for a variable-height `ScrollView` (`setRowHeights()`) |
| Lists | `.eden-listrow--selectable` in a natively scrolling `.eden-content` + `.eden-scrollbar` | `GLW::ListRow` (same tint + 4u lime bar) in a `GLW::ScrollView` (Stage 5.6). **Row-snapped**: the kit cannot clip (no scissor through the translator's letterboxed, render-scaled path), so it scrolls whole rows — by drag, by the scrollbar (thumb drag, track tap pages), or by the wheel through `eden_ui_take_wheel()` |
| Tabs (a few words) | `.eden-btn--tab` tiles; selected = pressed | `GLW::TabRail` (Stage 5.9): a HORIZONTAL row of equal RAISED tiles, the selected one PRESSED. The world browser's source tabs and the servers' Featured / Recent. The vertical icon rail (`.eden-tabrail`, the settings screen's) is `GLW::IconRail` (U.1): 64u icon tiles in a 72u translucent rail, selected PRESSED, disabled tiles muted and inert, scrolling by whole tiles with no visible bar when there are more than fit. Since U.3 the GL `SettingsMenu` is built on it (7 tabs, `TwoLineRow`s in a variable-height `ScrollView`, a fixed panel) |
| Progress | `.eden-progress` + `__fill` | `GLW::progressBar(rect, frac)` (Stage 5.9): SUNKEN track, lime 12% fill; `frac < 0` is an indeterminate block (preview loading) |
| Button tones / disabled | `.eden-btn--positive` / `--danger`, `:disabled` | `Button::setTone(TONE_POSITIVE / TONE_DANGER)` (same faces), `Button::setEnabled(false)` — solid chrome, grey label, no hit (the "not a translucent ghost" rule) |
| Text field caret | the browser's | **a glyph** — the text is rasterised as `"name|"` (chosen before the seam could measure, when a drawn caret would have sat at an estimated x). Editing is at the end only |

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

- New dimension? `du(N)` in mockup pixels.
- New surface? Compose a `WINDOW` + `CONTENT` + existing `GLW::` controls. If you think you need a
  new bevel, you almost certainly need RAISED, SUNKEN or PRESSED.
- New control that holds a value? It is SUNKEN.
- New widget? Add it to `GLWidgets`, not to a screen (docs/ui.md "Building blocks").
- New icon? Add the Lucide path to `../tools/eden-icons.js` and re-run `ui-icons.py` (regenerates
  `media/ui/ui_icons.png` and `Classes/UIIcons.h`). Never a *new* PNG; the only raster allowed is art
  the engine already ships.
- New setting row? Add it to `kSettings[]` in `src/seam/Settings.cpp`; the GL Settings screen
  renders the table generically.
