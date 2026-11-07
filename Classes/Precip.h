//  Precip.h — Stage AT.2b: rain (WORKING/atmosphere-plan-2026-11-05.md §9 P1, P2, §11 AT.2b).
//  Engine doc: docs/lighting-liquids-effects.md "Weather" > Rain.
//
//  One draw a frame (R::drawPrecip, program P_PRECIP): a static buffer of streak seeds uploaded once,
//  placed and animated by the vertex shader in a BOX x BOX_H x BOX box round the camera. The CPU sends a
//  few uniforms; nothing is simulated or streamed. Shelter is a BOX x BOX column-top cache (a TF_L8
//  texture, by world (x, z) mod BOX): a column is scanned only when it enters the box, when a column lands
//  (Terrain::addChunk) or when Terrain::setLand changes its top, and only the changed texture rows upload.
//  A streak whose head is below its column's top is collapsed in the shader, so rain stops at roofs,
//  under trees and in caves.
//
//  AT.2c splashes (High, atmo.splash): the same program, seed buffer and cache in splash mode, a second
//  draw of the first SPLASH_HIGH x precipAmount seeds as flat quads on their columns' tops. No new
//  resource: Low or atmo.splash off simply skips the draw.
//
//  Off = absent (P12): with Video > Atmosphere Off, atmo.precip off or Gameplay > Weather off, render()
//  frees the texture and the buffer and does nothing else; the hooks return at once.
#ifndef Precip_h
#define Precip_h

namespace Precip {
    enum {
        BOX = 32,          // the box and the cache, in columns (a power of two: the slot is (x & 31, z & 31))
        BOX_H = 32,        // the box's height (blocks): BELOW under the eye .. BOX_H - BELOW above it
        BELOW = 12,
        CAP_LOW = 1500,    // streaks at full intensity, per tier (plan §11)
        CAP_HIGH = 4000,
        SPLASH_HIGH = 1200,  // AT.2c: splash seeds at full intensity (High only; a quarter of them show at a time)
    };
    const float FALL_SPEED = 14.0f;   // blocks a second
    const float SNOW_SPEED = 2.2f;    // AT.3: flakes fall slowly
    const float STREAK_LENGTH = 0.9f;

    // Once a frame from World::update (after Weather::update): the motion clock. Stops under the pause menu.
    void update(float etime, bool paused);
    // Once a frame from World::renderFrame's effects pass, under the camera's modelview (render-local
    // coordinates, not the effects' world translation).
    void render();
    // Terrain hooks. setLand: after the GBLOCK write (y vertical, Terrain order). landed: world chunk column.
    void onSetLand(int x, int z, int y, int type);
    void onLanded(int cx, int cz);
    // Terrain::deallocateMemory / clearBlocks: free everything (the next render() rebuilds from scratch).
    void reset();

    // ---- diagnostics (--weather-selftest=precip) --------------------------------------------------
    bool live();           // the texture and the buffer exist
    int drawn();           // streaks in the last render() (0 when nothing was drawn)
    int top(int x, int z); // the cached top of world column (x, z); -1 when it is not cached
    long scans();          // columns scanned since the start
    long rowUploads();     // texture rows uploaded since the start
    int cap();             // the tier's cap (0 when off)
    int splashes();        // splash seeds submitted in the last render() (AT.2c; 0 when none: Low, atmo.splash off, no rain)
}

#endif
