//  Ambient.h — Stage AT.7: ambient particles (WORKING/atmosphere-plan-2026-11-05.md §5, S2, §11 AT.7).
//  Engine doc: docs/lighting-liquids-effects.md "Ambient particles".
//
//  Fireflies (night, over grass and flowers), falling leaves (under leaf blocks), dust motes (daylight),
//  embers (above lava) and bubbles (camera in a liquid). An own fixed pool (High <= 256, Low <= 96; never
//  `pbuffer`, never grows), simulated on the CPU, drawn as one R::drawSprites batch streamed from the
//  pool's client memory. AT.11: points are world-sized (size / distance); spawning remembers the grass, leaf
//  and lava cells its samples find and emits from them steadily; the only R resource is the 16 x 16 glow
//  texture of renderHalos(), made on first use and freed when nothing glows. Spawning samples at most SAMPLES random cells a
//  frame within RADIUS of the camera (never a scan). Needs Video > Atmosphere (not Gameplay > Weather);
//  atmo.ambient is the element switch. Off = absent (P12): the pool is emptied, nothing else happens.
#ifndef Ambient_h
#define Ambient_h

namespace Ambient {
    enum Kind { FIREFLY, LEAF, MOTE, EMBER, BUBBLE, KINDS };
    enum {
        CAP_LOW = 96,
        CAP_HIGH = 256,
        POOL = CAP_HIGH,   // the fixed array
        SAMPLES = 8,       // random cells looked at a frame
        PROBES = 2,        // AT.11: random columns probed a frame (finds the grass / leaf / lava sources)
        RADIUS = 12,       // horizontal spawn range; a particle past RADIUS + 4 is dropped
    };

    // Once a frame from World::update (after Weather::update). Stops under the pause menu.
    void update(float etime, bool paused);
    // Once a frame from World::renderFrame's effects pass, next to Precip::render (render-local coordinates).
    void render();
    // AT.8 (plan S1), redesigned in AT.11: one additive batch of camera-facing glow quads with a 16 x 16 pixel-ring
    // texture: a steady halo on each recorded TYPE_LIGHTBOX cell near the camera at night (High + atmo.halos,
    // <= HALO_MAX, nearest first, tinted by the lamp's paint, sized in world units) and a small glow on each
    // firefly. Faded by the world fog's range on the CPU. Called right after render().
    enum { HALO_MAX = 64, HALO_RADIUS = 40 };
    void renderHalos();
    int halosDrawn();           // lamp halos in the last renderHalos()
    int glowsDrawn();           // firefly glows in the last renderHalos()
    long glowUploads();         // glow texture creations since the start
    bool glowLive();            // the glow texture exists
    // AT.11: seconds of play (wrapped, stops when paused): the water sparkle's clock (Graphics::applyWaterTint).
    float clock();
    // Terrain::deallocateMemory / clearBlocks, and a world change: empty the pool.
    void reset();

    // ---- diagnostics (--weather-selftest=ambient) ---------------------------------------------------
    int cap();                  // the tier's cap (0 when off)
    int count();                // live particles
    int count(Kind k);
    int drawn();                // particles in the last render()
    long spawned();             // spawned since the start
    bool spawnAt(Kind k, float x, float y, float z);   // test: force one (false when the pool or its cap is full)
}

#endif
