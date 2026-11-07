//  SkyCycle.h — Stage DN (day/night cycle; WORKING/day-night-cycle-plan-2026-10-31.md).
//  DN.1: the one night predicate and the one re-mesh request. DN.2: the clock, its lock and its
//  `.emod` record (SKY_CLOCK, type 11; docs/emod-file-format.md). DN.3: the sky look (gradient, glow, stars).
#ifndef SkyCycle_h
#define SkyCycle_h

#include <stddef.h>
#include <stdint.h>
#include "Vector.h"

// True when the world is "night" (lamps lit, skylight 0.35, night ambience). Cycle off: the painted sky is
// colorTable[54] (sky_paintedNight). Cycle on (DN.5): the clock's night state, which flips with
// hysteresis around a sun elevation of -4 degrees (plan D4). Use this instead of testing final_skycolor.
bool sky_isNight();
// The painted (region) sky's night bit, whatever the cycle does: what paintSky / setSky / the region walk
// compare a new sky colour against. Equal to sky_isNight() while the cycle is off.
bool sky_paintedNight();

// Re-mesh the whole resident window (a night flip changes every baked skylight). No-op on LOW_MEM_DEVICE.
void sky_requestNightRemesh();

// The clock (plan D2/D3/D7/D8). t in [0,1): 0 midnight, 0.25 sunrise, 0.5 noon, 0.75 sunset.
namespace SkyCycle {
    enum { RECORD_BYTES = 16, FLAG_LOCKED = 1 };
    const double DEFAULT_TIME = 0.30;   // a world with no SKY_CLOCK record starts in the morning

    // Gameplay > Day/night cycle, and Day length (minutes of real time per full day).
    void setEnabled(bool on);
    bool enabled();
    void setDayLengthMinutes(int minutes);
    double dayLengthSeconds();

    // The time verbs (`day`, `time T`, `world.time`) reach setTime through CmdScript::timeHook's
    // default while the cycle is on, and the stock setSky repaint while it is off (D8).
    double time();
    void setTime(double t);             // wraps into [0,1); allowed while locked (D7)
    bool locked();
    void setLocked(bool on);            // `timefreeze`, World Properties, SKY_CLOCK flags bit 0: one flag

    // Once a frame from World::update in play. Advances t by etime / dayLength unless the cycle is
    // off, the clock is locked, or `paused` (the pause menu is up).
    void update(float etime, bool paused);

    // Per world. reset(): no record (a new world, a world from before DN, a multiplayer session).
    // load(): a SKY_CLOCK payload; anything that is not a valid 16-byte record is ignored (reset).
    void reset();
    void load(const uint8_t* rec, size_t n);
    // The payload a save writes, and whether it has to: true once the clock or its lock moved since
    // the world loaded or last committed it. saved() after the batch commits.
    void encode(uint8_t out[RECORD_BYTES]);
    bool needsSave();
    void saved();
    // The decoder the store, the selftest and emod.py agree on. False: not a valid record.
    bool decode(const uint8_t* rec, size_t n, double* t, bool* locked);

    // Unit vector toward the sun (y up); the moon is its negation. For DN.3/DN.4 and DN.F.
    Vector sunDir();

    // ---- DN.5: lighting (plan section 4) --------------------------------------------------------
    // Sun elevation in degrees (-70..70 with the 20-degree tilt).
    float sunElevationDeg();
    // L(t): the sky light a day-baked mesh is multiplied by. 1.0 above 15 degrees, easing to exactly
    // NIGHT_SKYLIGHT at -4 degrees, then down to NIGHT_SKYLIGHT * 0.75 by -18. 1.0 while the cycle is off.
    const float NIGHT_SKYLIGHT = 0.35f;   // rebuild2's night skylight (TerrainChunk.cpp)
    float skyLight();
    // R::setWorldLight for a mesh baked with that night bit: L (day) or L / 0.35 (night), times the
    // time-of-day colour (warm near the horizon, cool at night). A day-baked chunk and a night-baked one
    // then look the same away from lamps, so the flip's re-mesh streams in without a patchwork.
    // (1,1,1) while the cycle is off. AT.2a: times Weather::lightMul() while Gameplay > Weather is on
    // (exactly 1.0 in clear weather), so with the cycle off it is (1,1,1) x the weather.
    void worldLight(bool bakedNight, float out[3]);
    // AT.2a: whether the world-light uniform is in use at all (the dnLight gate in Terrain.cpp): the
    // cycle or the weather is on. Both off: never set, (1,1,1), exactly stock.
    bool lightActive();
    // A stock day/night light pair (door, golden cube, creature LightSets) for the clock: blended from
    // day to night as L falls to 0.35, then night scaled by L / 0.35, times the colour. Cycle off: the
    // stock pick (night when !LOW_MEM_DEVICE && sky_isNight()). rgb only.
    void blendLight(const float day[3], const float night[3], float out[3]);
    // Night flips the watcher has requested (a full-window re-mesh each); --sky-shot's gate counts them.
    int nightFlips();

    // DN.4: the pixel sun and moon, called by Terrain::render's sky pass (ortho, after the gradient).
    // Does nothing while the cycle is off.
    void drawSunMoon();
}

#endif
