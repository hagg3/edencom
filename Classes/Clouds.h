//  Clouds.h — Stage AT.6: the pixel cloud layer and its shadows (WORKING/atmosphere-plan-2026-11-05.md §5,
//  §9 P3, P5, §11 AT.6). Engine doc: docs/lighting-liquids-effects.md "Weather" > Clouds.
//
//  One horizontal layer CLOUD_ABOVE blocks above max(T_HEIGHT, CLOUD_FLOOR) (no built block reaches it), drawn by Terrain::render
//  straight after the sky (so after the opaque terrain), as one viewport quad a hair nearer than the sky and
//  the sun with a depth test: only visible sky pixels pay (P5). Each fragment intersects its view ray with the
//  plane and samples a 128 x 128 TF_L8 tileable cloud map (generated at first use, no asset), nearest, so
//  each texel is CELL x CELL blocks of cloud or sky: the pixel look. A texel is cloud when it is above
//  1 - coverage; the map's values are rank-equalised, so the coverage is the cloud fraction exactly.
//  Coverage, tint and drift are uniforms: base coverage BASE_COVER with the weather off or clear, up to ~0.95
//  in a storm; the colour follows the world light (the clock and the overcast) and, with the cycle on, the
//  horizon colour (pink at dawn, dark at night); the wind scrolls the map.
//
//  Cloud shadows (High + atmo.cloud_shadows): applyShadow() states R::setCloudShadow for the terrain passes,
//  the EDEN_CLOUD_SHADOW vertex variant of program 1 (P3): each vertex samples the same map where the ray
//  toward the sun meets the plane, so the shadows match the layer overhead and move with it. Only by day
//  and fading out as the sky closes over (overcast is already dark).
//
//  Off = absent (P12): Atmosphere Off, or atmo.clouds and atmo.cloud_shadows both off, frees the map and
//  the quad's buffer at the next render(); nothing else runs.
#ifndef Clouds_h
#define Clouds_h

namespace Clouds {
    enum {
        TEX = 128,          // the map, texels a side (POT: repeat)
        CELL = 8,           // blocks per texel (AT.11: was 12)
        CLOUD_FLOOR = 128,  // the plane is CLOUD_ABOVE over the larger of T_HEIGHT and this: y 192 in a 64z
        CLOUD_ABOVE = 64,   // world and 320 in a 256z one (user 2026-11-06: y 88 at 64z looked too low)
    };
    const float BASE_COVER = 0.30f;   // the coverage with the weather off or clear

    // Once a frame from World::update (after Weather::update): the wind. Stops under the pause menu.
    void update(float etime, bool paused);
    // Terrain::render's sky pass, after the sky, the sun and the moon (the camera's projection and modelview,
    // the blocks' x0.25 included, are current). Also frees everything once it is all off.
    void render();
    // Graphics::beginTerrain and Terrain::render2 (before the blocks' x0.25 push: the modelview is the
    // camera's view): R::setCloudShadow when cloud shadows are on and the sun is up, else R::cloudShadowOff.
    void applyShadow();
    // Terrain::deallocateMemory: free everything (the next render() rebuilds it).
    void reset();

    // ---- diagnostics (--weather-selftest=clouds) ------------------------------------------------------
    bool live();            // the map and the buffer exist
    bool drawn();           // the layer was drawn in the last render()
    bool shadowStated();    // the last applyShadow() stated R::setCloudShadow
    float coverage();       // what the layer and the shadows use now (0 when off)
    float shadowStrength(); // the last stated shadow strength (0 when off)
    long texUploads();      // map uploads since the start (one per creation)
    float height();         // the plane's world y
}

#endif
