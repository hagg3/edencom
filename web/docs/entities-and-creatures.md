# Entity System / Creatures (Web Port)

Read [`../../docs/entities-and-creatures.md`](../../docs/entities-and-creatures.md)
first — `Model.mm`'s AI, animation state machine, and save/restore logic are
unmodified. The only web-specific work here was making creatures **render** at all,
which was the port's last real feature gap (closed once, in a single pass) and lives
almost entirely in the GL shim — see [gl-shim.md](gl-shim.md) for the mechanism
(`GL_OES_matrix_palette` emulation as a second GLSL ES 3.00 program, the
`pvrt_matrix_palette.cpp` wrap, and the eye-space palette-matrix math).

## What made it work, engine-side
Nothing — no `Classes/` edit. Four purely additive pieces: the GL shim's matrix-palette
group, the `--wrap` on `CPVRTglesExt::LoadExtensions`, preloading the 7 `.pod` models
flat to `/bundle/*.pod` (matching where `LoadModels`'s `CPVRTResourceFile` looks,
relative to the shimmed `NSBundle` resource path), and `GL_LIGHT0` tracking applied
only on this rendering path (see [lighting-liquids-effects.md](lighting-liquids-effects.md)).

## The bug headless testing couldn't catch
Creature skin PNGs live in `media/models/` alongside the `.pod` files. Only the PODs
were initially preloaded, so `Resources::getTex(...)` for a creature skin came back
with texture name 0; binding an unbound texture unit samples `(0,0,0,1)` with **no GL
error anywhere** — creatures rendered as pure black silhouettes. This was only found
via a temporary `fprintf` dump of texture state, consistent with
[conventions-and-pitfalls.md](conventions-and-pitfalls.md) #3 ("measure, don't
reason") — a headless numeric probe of model/frame counts looked completely healthy
the whole time. Fixed by also preloading `media/models` (not just the `.pod`s) in the
asset packaging step — see [resources-and-audio.md](resources-and-audio.md).
