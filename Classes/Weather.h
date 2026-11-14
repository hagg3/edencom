//  Weather.h — Stage AT (atmosphere; WORKING/atmosphere-plan-2026-11-05.md §10–§11).
//  AT.1b: the weather state, its schedule, the region/snowline rule and the `.emod` record 12 WEATHER
//  (32 B, codec 0; docs/emod-file-format.md "WEATHER"). Nothing here draws: AT.2+ read lightMul(),
//  skyGrey(), fogScale(), precipAmount() and precipAt().
#ifndef Weather_h
#define Weather_h

#include <stddef.h>
#include <stdint.h>

// The default map's 4x4 region grid (the bed-ambience picker in Player.cpp uses the same cells):
// px, pz in 0..4 from world x, z. Returns true when the regions mean something (the default seed).
// (0,0) is the snow mountain, (0,1) the pyramid, (0,2) the oasis.
bool eden_default_region(float x, float z, int* px, int* pz);

// AT.1a: the Atmosphere element switches (the argument of eden_atmo_element(), the index of the seam's
// kAtmoKey[] / kAtmoTier[] in port/src/seam/Settings.cpp). Insert only at the end, before AT_COUNT.
enum { AT_PRECIP, AT_SPLASH, AT_CLOUDS, AT_CLOUD_SHADOWS, AT_HEIGHT_FOG, AT_AMBIENT, AT_WATER_TINT, AT_GLINT, AT_HALOS, AT_COUNT };

namespace Weather {
    // A spell's kind (the record's byte 0). Snow and dry are where, not what (plan A5): precipAt().
    enum Kind { CLEAR = 0, OVERCAST = 1, RAIN = 2, STORM = 3, KIND_COUNT = 4 };
    enum Precip { PRECIP_NONE = 0, PRECIP_RAIN = 1, PRECIP_SNOW = 2, PRECIP_DRY = 3 };
    enum { RECORD_BYTES = 32, FLAG_LOCKED = 1, FLAG_SNOW = 2 };
    const uint64_t SEED_SALT = 0x57454154u;   // "WEAT": absent record -> RNG from LEVEL_SEED ^ this
    const float SWING_SECONDS = 45.0f;        // intensity moves 0 -> 1 in this many game seconds
    const double NO_CYCLE_DAY_SECONDS = 20 * 60;   // a "day" for spell lengths while the cycle is off

    // Gameplay > Weather (eden_weather_enabled(), which --weather= overrides). Read once a frame by
    // update(); off = update() returns at once, nothing advances, no record is ever wanted.
    bool active();

    // Once a frame from World::update in play, after SkyCycle's step. Stops under the pause menu.
    void update(float etime, bool paused);

    // Per world. reset(): no record (a new world, a world from before AT, a multiplayer session):
    // clear, intensity 0, the RNG seeded from LEVEL_SEED ^ SEED_SALT on the first update. resetSeed():
    // the same with an explicit seed (the selftest). load(): a WEATHER payload; anything decode()
    // refuses is reset().
    void reset();
    void resetSeed(uint32_t levelSeed);
    void load(const uint8_t* rec, size_t n);
    // What a save writes, and whether it has to (plan P7): true only when the kind or the flags
    // changed, the seconds left moved >= 30 s or the intensity >= 0.1 since the load or the last
    // commit. Never true while --weather= forces a kind, or before the weather ever ran.
    void encode(uint8_t out[RECORD_BYTES]);
    bool needsSave();
    void saved();
    struct State {
        int kind;              // Kind
        int flags;             // FLAG_*
        float intensity;       // [0,1]
        float secondsLeft;     // > 0
        uint32_t spells;
        uint64_t rng;
    };
    // The decoder the store, the selftest and emod.py agree on. False: not a valid record
    // (wrong length, kind > 3, intensity outside [0,1], seconds left not finite and > 0).
    bool decode(const uint8_t* rec, size_t n, State* out);
    State state();

    // ---- the state, and the verbs AT.1c wires to the console / World Properties / Lua -------------
    Kind kind();               // the current spell's target
    float intensity();         // eased; 0 clear .. 1 a full storm
    float secondsLeft();
    bool locked();             // a locked spell never ends (the intensity still eases)
    void setLocked(bool on);
    bool forceSnow();
    // Start a spell of `k` now (secs <= 0: a drawn length). snow: precipitation is snow everywhere
    // until the next set() or the end of this spell (flags bit 1).
    void set(Kind k, float secs, bool snow);
    // QA.25: the one name table. name(k) is "clear".."storm" (NULL out of range). kindByName takes those names
    // and "snow" (a where, not a kind: returns RAIN and sets *snow); -1 for anything else. setByName is the
    // console / Lua / cvar path: false (nothing changed) for an unknown name, else Weather::set().
    const char* name(int k);
    int kindByName(const char* s, bool* snow);
    bool setByName(const char* s, float secs);
    // The kind's intensity target: clear 0, overcast 0.4, rain 0.7, storm 1.
    float targetFor(Kind k);
    // A spell's mean length in days (clear 0.5, overcast 0.2, rain 0.15, storm 0.06; drawn from
    // [0.5, 1.5) x mean) and the next-spell matrix (row = this kind; only neighbours on clear - overcast -
    // rain - storm, a repeat allowed). The defaults give time shares clear 0.60, overcast 0.20, rain 0.15,
    // storm 0.05 (plan §10.3). AT.9 tunes these.
    void setTransitions(const float m[KIND_COUNT][KIND_COUNT]);
    void setMeanDays(const float d[KIND_COUNT]);
    // The length of a day in game seconds, for spell lengths: SkyCycle's while the cycle is on, else
    // NO_CYCLE_DAY_SECONDS.
    double daySeconds();

    // ---- what the look rows read (all exactly the identity when the intensity is 0) ---------------
    float lightMul();          // world-light multiplier: 1 clear, 0.7 overcast, 0.55 storm (x flashMul())
    float skyGrey();           // 0..1 blend of the sky toward grey
    float clearSky();          // 1 clear .. 0 fully overcast: the sun, moon, stars and glow fade by it
    // AT.2a: blend an RGB sky/fog colour toward its overcast grey (its luma x 0.8) by skyGrey(). Exactly
    // the input when the weather is off or the intensity is 0.
    void greySky(float rgb[3]);
    // AT.5: multiplies the fog start/end (Graphics::applyWorldFog, every tier): 1 clear or weather off,
    // 0.9 overcast, 0.6 in a full storm, and x down to 0.7 more in the dawn mist.
    float fogScale();
    // AT.5: mist 0..1, the larger of the dawn mist (a bump around sunrise, SkyCycle time 0.19-0.33, only
    // while the cycle runs) and the rain mist (precipAmount()). 0 with the weather off.
    float dawnMist();
    float mist();
    // AT.5: the High tier's valley mist (R::setHeightFog's params: top y, 1 / thickness, 1 / distance to
    // full, strength 0.85 x mist()). False (nothing to draw) when mist() is 0.
    bool heightFog(float params[4]);
    // The mist's top as a fraction of T_HEIGHT (cvar weather.mistheight, default 0.4).
    void setMistHeight(float frac);
    float mistHeight();
    float precipAmount();      // 0 until the intensity passes overcast's, 1 in a full storm
    // What falls at world cell (x, z, y) (Terrain order, y up): NONE when precipAmount() is 0; SNOW when
    // forced, in the default map's snow-mountain region, or at/above the snowline; DRY in the default
    // map's pyramid and oasis regions; else RAIN.
    Precip precipAt(float x, float z, float y);
    // ---- AT.4: lightning (plan A9). Only in a storm at full strength, and not in snow (a cold storm is heavy
    // snow without lightning). A strike is a flash (world light + sky x flashMul(), peak 1.5, rise 1 frame,
    // fade 0.3 s, now and then a 60 % second flash 0.4 s later; strikes are >= 4 s apart so <= 2 flashes/s)
    // and a thunder one-shot 0.5-4 s later (close -> distant variation by delay). Video > Reduce flashing
    // swaps the flash for a dim: flashMul() dips to 0.85 and back over 0.5 s, never above 1.
    // Dry regions (plan A5): the same storm, but strikes are rarer and the thunder is always distant.
    // Not saved, not part of the schedule (its own RNG), so the record and the schedule are unchanged.
    // 1 outside a strike, with the weather off, or when lightning is not eligible.
    float flashMul();
    // Called by update(); public for the selftest. eligible: the storm is on and the camera is not in snow.
    void lightningUpdate(float etime, bool eligible, bool dry);
    // Strikes since the last lightningReset(), and the thunder variation (0 close .. 2 distant) of the last one.
    int strikeCount();
    int lastThunderVariation();
    int thunderPlayed();       // one-shots that reached the sound hook
    void lightningReset();     // new world / selftest: no strike, no thunder pending, strike RNG reseeded
    // The sound hook (default: Resources::playThunder). The selftest swaps it to count.
    void setThunderHook(void (*hook)(int variation));
    // SX.9: Settings rows over the schedule. Frequency 0 Rare / 1 Normal / 2 Often scales clear spells x2 / x1 / x0.4;
    // storms 0 Off / 1 Rare / 2 Normal / 3 Often scales the rain -> storm weight x0 / x0.4 / x1 / x2.5. Normal/Normal
    // is the unchanged schedule (shares 0.60 / 0.20 / 0.15 / 0.05). A storm already running ends as drawn.
    void setFrequency(int level);
    void setStorms(int level);
    // The snowline as a fraction of T_HEIGHT (cvar weather.snowline, default 0.75).
    void setSnowline(float frac);
    float snowline();
}

#endif
