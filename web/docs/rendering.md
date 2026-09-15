# Rendering (Web Port)

Read [`../../docs/rendering.md`](../../docs/rendering.md) first — the draw passes,
chunk meshing (`rebuild2`/`prepareVBO`), and vertex formats are unmodified; they're
still produced by the same `TerrainChunk.mm`/`Terrain.mm`/`Graphics.mm` code. What
changes is entirely underneath: every ES 1.1 fixed-function call those files make now
goes through a shim into WebGL2 rather than a real ES1 driver.

**The GL shim itself has its own dedicated doc — [gl-shim.md](gl-shim.md) — because
it's the single largest body of web-specific knowledge in this port.** Read it before
touching anything rendering-related: matrix-stack init, the two-GLSL-program split
for creature skinning, the draw-state dirty-tracking cache, `GL_ARRAY_BUFFER`/element
buffer tracking, context-loss handling, the dynamic-drawable-vs-fixed-screen-space
split, and the mobile touch-offset/picking-viewport fix all live there.

## Known inefficiency, not yet addressed
`TerrainChunk.mm` issues 6 separate `glDrawArrays` calls per visible chunk (one per
face direction) — an ES1-era artifact of how the mesher buckets faces. This could be
coalesced entirely inside the shim without touching `Classes/`, but hasn't been.

## Retina/low-res toggle is inert
`EdenViewController_web::drawFrame`'s "drop to low-res" branch flips the relevant
globals but never actually recreates the drawable at a new resolution — the engine's
own quality-toggle request is currently a no-op on web.
