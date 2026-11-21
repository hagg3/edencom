//  SunShadow.h — Stage SH.3: GPU sun shadows near the player. Engine doc: docs/lighting-liquids-effects.md
//  "Sun shadows"; the renderer half (depth target, depth programs, the EDEN_SUN_SHADOW lookup) is
//  Classes/Renderer.h and port/docs/renderer.md.
//
//  One orthographic shadow map looking down the sun, centred on the eye snapped to `shadow.snap` blocks, at
//  1/8 block a texel (2048 texels on desktop, 1024 on iOS: 256 / 128 blocks across). It is re-rendered only
//  when it is dirty: created, a tunable changed, the eye moved more than a snap from the centre on an axis
//  (hysteresis: crossing a cell edge and back costs nothing), the quantised sun moved (one step
//  of `shadow.sun_step` degrees of arc), or a re-meshed chunk overlaps its box (noteChunk). The depth pass
//  runs in World::renderFrame before the scene pass (a framebuffer switch inside it costs iOS a tile flush)
//  and draws every resident chunk whose box overlaps the map's, behind the camera too: the sun-facing face
//  buckets of the opaque stream (+ the angled bucket 6), and of stream 2 alpha-tested on atlas2 (leaves and
//  glass cast their shape, water casts nothing). apply() states R::setSunShadow for the terrain passes.
//
//  Off = absent: the setting off, the Air 2 class (the row is hidden), an incomplete depth framebuffer, or a
//  pinned harness mode: no texture, no framebuffer, no depth program, no caster walk, no uniforms. While on,
//  the baked top-face shadows (Shadow.h) are replaced: the mesher runs with them Off. At night and under an
//  overcast sky the strength is 0: no pass and no lookup (the map is kept for the morning).
#ifndef SunShadow_h
#define SunShadow_h

namespace SunShadow {
    // Video > Sun shadows (the seam passes it already gated by the device: the Air 2 class is always off).
    void setEnabled(bool on);
    // --sunshadows=off|on: -1 none, 0 off, 1 on. Not stored. The scripted harness modes set 0 unless asked.
    void setOverride(int v);
    int  override();
    bool wanted();          // the setting (or the override) asks for them and the driver has not refused
    // World::renderFrame, before R::beginScene: re-render the map when it is dirty. Frees it when off.
    void renderMap();
    // Graphics::beginTerrain and Terrain::render2, before the blocks' x0.25 (the modelview is the camera's
    // view): R::setSunShadow while there is a map and the sun is up, else R::sunShadowOff.
    void apply();
    // TerrainChunk::prepareVBO / dropGeometry: a chunk's geometry changed (pbounds: its absolute box).
    void noteChunk(const int pbounds[6]);
    // Terrain::allocateMemory / deallocateMemory / clearBlocks: the window changed under the map. Frees it.
    void reset();

    // shadow.* (Console): advisory tuning, not stored. Each change dirties the map.
    struct Tunables {
        float distance;     // blocks from the eye that are shaded (clamped to the map and the window)
        float strength;     // how dark: colour x (1 - strength)
        float reachFull;    // blocks along the sun ray behind the first surface: full shadow up to here,
        float reachEnd;     //   none from here (deep caves stay as bright as without sun shadows)
        float sunStep;      // degrees of arc the sun moves between re-renders
        float snap;         // the centre's grid, and how far the eye moves from it before a re-render
    };
    Tunables& tunables();
    void tunablesChanged();

    // ---- diagnostics (--sunshadow-selftest) --------------------------------------------------------
    bool live();            // the map texture exists
    bool stated();          // the last apply() stated R::setSunShadow
    long renders();         // depth passes run since the start
    int  lastCasters();     // chunks the last pass drew
    bool lastCasterHad(const int pbounds[6]);   // a chunk with that box was among the last pass's casters
    float strengthNow();    // the strength the last renderMap() worked out (0 at night / overcast / off)
    int  mapSize();         // texels a side
    void setTestMapSize(int n);   // a smaller map (0 = the platform's), so a meshed chunk can be outside it
}

#endif
