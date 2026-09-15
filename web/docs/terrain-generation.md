# Terrain Generation (Web Port)

**Identical to the root docs — see
[`../../docs/terrain-generation.md`](../../docs/terrain-generation.md).**

The offline generator (`TerrainGen2.mm`) and runtime flat generator
(`TerrainGenerator.mm`) are unmodified and, as on iOS, `JUST_TERRAIN_GEN` builds are
not something this port runs day to day — the handful of CoreGraphics symbols that
path drags in are stubbed only so the build links
(`src/shim/foundation/uikit_stubs.{h,mm}`), not because the port exercises worldgen.
At runtime the web port streams from the same bundled `Eden.eden` at
`/bundle/Eden.eden`, resolved through `NSBundle` (shimmed), exactly like the native
build's bundle-streaming path. Since pass 46 the file is not resident: it is a lazy
FS node reading 32 KB blocks over HTTP Range requests, which is transparent to
`fmh_readColumnFromDefault` but does mean a column read can block on a network round
trip — see [save-load.md](save-load.md)'s "The bundled default world" section. That
32 KB block granularity is also why the column read is deliberately *not* greedy: a
read-ahead past what the caller asked for costs a real copy out of the block cache, and
a fixed 16 KB read-ahead in the `NSFileHandle` shim measured as a **60% regression** on
this path (B6, 2026-08-28) even though it cut the number of `fread`s. The win came from
the caller asking for the right amount once, not from buffering underneath it.
