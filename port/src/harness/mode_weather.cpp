// AT.1a: --weather-selftest[=leg], the atmosphere stage's gate (WORKING/atmosphere-plan-2026-11-05.md §11).
// Leg `settings` (AT.1a): the Weather / Atmosphere / Reduce flashing rows, the tier -> element rows (P10), the
// per-device default (A7), the preset sync, the overrides (P11) and the atmo.* cvars.
// AT.1b (Classes/Weather): `schedule` (determinism from a seed, the clear-overcast-rain-storm walk, Day length
// scaling, easing, pause/off, the look identities), `region` (plan A5's snow/dry/snowline rule), `record` (the
// 32-byte WEATHER codec, malformed records, needsSave's coarse steps), `save` (record 12 through a real save and
// reload, P7: no save wake, off = no record, a pre-AT world loads clear).
// AT.2b (Classes/Precip): `precip`: off = absent (no texture, no buffer), the tier caps x intensity, G3 (600 frames
// of rain: nothing allocated, nothing re-meshed, no rescans or uploads while standing still), the column-top
// cache under a placed and removed roof, the cache following a move, and freed again when the tier goes Off.
// AT.5: `mist` (the fog scale and its identities, dawn mist, height fog on High only, culling past the fog, G3).
// AT.6: `clouds` (off = absent, the layer on Low and High, shadows on High by day only, coverage from the weather,
// G3: 600 frames with no upload, allocation or re-mesh, freed at Off).
// Headless; prefs are ephemeral, so nothing is stored.
#include "harness.h"
#include <cmath>
#include <algorithm>
#include <vector>
#include "../../../Classes/Weather.h"
#include "../../../Classes/SkyCycle.h"
#include "../../../Classes/ModScript.h"
#include "../../../Classes/Console.h"
#include "../../../Classes/Precip.h"
#include "../../../Classes/Clouds.h"
#include "../../../Classes/Ambient.h"
#include "../../../Classes/Camera.h"
#include "../../render/render_internal.h"
#include "../../../Classes/Graphics.h"
int eden_lamp_positions(float ex, float ey, float ez, float radius, int cap, float (*out)[3]);   // Terrain.cpp (AT.8; AT.11 halos leg)

long eden_mist_culled(void);   // Terrain.cpp (AT.5)

using namespace harness;

extern "C" {   // port/src/seam/Settings.cpp (AT.1a)
int  eden_atmo_tier(void);
int  eden_atmo_element(int e);
void eden_atmo_set_override(int tier);
int  eden_atmo_override(void);
void eden_weather_set_override(int kind);
int  eden_weather_override(void);
int  eden_weather_enabled(void);
int  eden_reduce_flashing(void);
}

namespace harness {
namespace g_weather {

static int row(const char* key) {
    for (int i = 0; i < eden_settings_count(); ++i) if (!std::strcmp(eden_settings_key(i), key)) return i;
    return -1;
}
static const char* const kEl[9] = {"precip", "splash", "clouds", "cloud_shadows", "height_fog", "ambient", "water_tint", "glint", "halos"};
static const int kLow[9] = {1, 0, 1, 0, 0, 1, 1, 0, 0};

static void settings_leg() {
    char d[256];
    const int wi = row("weather"), ai = row("atmosphere"), ri = row("reduce_flashing"), pi = row("graphics_preset");
    check(wi >= 0 && ai >= 0 && ri >= 0 && pi >= 0, "weather, atmosphere, reduce_flashing rows exist", "");
    int elRow[9];
    bool all = true;
    for (int k = 0; k < 9; ++k) { elRow[k] = row(kEl[k]); all = all && elRow[k] >= 0; }
    check(all, "the nine atmo element rows exist", "");
    check(!std::strcmp(eden_settings_group(wi), "Gameplay") && !std::strcmp(eden_settings_group(ai), "Video") &&
              !std::strcmp(eden_settings_group(ri), "Video") && eden_settings_def(wi) == 0.0f && eden_settings_def(ri) == 0.0f,
          "Weather is Gameplay (default off), Atmosphere and Reduce flashing are Video", "");
    bool hidden = true;
    for (int k = 0; k < 9; ++k) hidden = hidden && eden_settings_native_hidden(elRow[k]) == 0;   // SX.5: on the Atmosphere details page
    check(hidden && !eden_settings_native_hidden(wi) && !eden_settings_native_hidden(ai) && !eden_settings_native_hidden(ri),
          "element rows are shown (Atmosphere details page) with the three user rows", "");

    // A7: the device default (this Mac: High). Unstored rows read it; the harness never runs on an Air 2.
    eden_atmo_set_override(-1);
    eden_weather_set_override(-1);
    std::snprintf(d, sizeof(d), "tier %d, weather %d, reduce %d", eden_atmo_tier(), eden_weather_enabled(), eden_reduce_flashing());
    check(eden_weather_enabled() == 0 && eden_reduce_flashing() == 0, "Weather and Reduce flashing start off", d);

    // P10: each tier writes the elements.
    for (int t = 2; t >= 0; --t) {
        eden_settings_set(ai, (float)t);
        bool ok = eden_atmo_tier() == t;
        for (int k = 0; k < 9; ++k) {
            const int want = t == 0 ? 0 : t == 1 ? kLow[k] : (k == 3 ? 0 : 1);   // High: all but cloud shadows (opt-in)
            ok = ok && eden_atmo_element(k) == want && (int)eden_settings_get(elRow[k]) == want;
        }
        std::snprintf(d, sizeof(d), "tier %d", t);
        check(ok, "a tier change sets the element rows (Off none, Low the Low set, High all but the opt-in cloud shadows)", d);
    }
    // A hand-set element stays until the next tier change; Off gates it (Off = absent).
    eden_settings_set(ai, 1.0f);
    eden_settings_set(elRow[3], 1.0f);
    check(eden_atmo_element(3) == 1, "an element switched on by hand reads on under Low", "");
    eden_settings_set(ai, 0.0f);
    check(eden_atmo_element(3) == 0 && eden_atmo_element(0) == 0, "tier Off gates every element", "");
    eden_settings_set(elRow[3], 1.0f);   // SX.5: a hand edit under Off becomes Custom, and Custom draws it
    check((int)eden_settings_get(ai) == 3 && eden_atmo_element(3) == 1, "an element switched on under Off reads Custom and draws", "");
    eden_settings_set(ai, 1.0f);
    check(eden_atmo_element(3) == 0, "the next tier change rewrites a hand-set element", "");

    // P11: --atmo= / --weather= override without storing.
    eden_settings_set(ai, 2.0f);
    eden_atmo_set_override(0);
    const float stored = eden_settings_get(ai);
    check(eden_atmo_tier() == 0 && eden_atmo_element(0) == 0 && stored == 2.0f, "--atmo=off overrides the tier and stores nothing", "");
    eden_atmo_set_override(1);
    check(eden_atmo_tier() == 1 && eden_atmo_element(2) == 1 && eden_atmo_element(1) == 0 && eden_atmo_element(7) == 0,
          "--atmo=low reads the Low table, not the stored elements", "");
    eden_atmo_set_override(-1);
    eden_weather_set_override(4);
    check(eden_weather_enabled() == 1 && eden_weather_override() == 4, "--weather=snow turns weather on for the run", "");
    eden_weather_set_override(0);
    eden_settings_set(wi, 1.0f);
    check(eden_weather_enabled() == 0, "--weather=off (the pin) beats the stored toggle", "");
    eden_weather_set_override(-1);
    check(eden_weather_enabled() == 1, "no override: the stored toggle", "");
    eden_settings_set(wi, 0.0f);

    // Presets: Low -> Off, High -> High, Default -> this device's default; moving the tier by hand drops to Custom.
    eden_settings_set(pi, 1.0f);
    check(eden_atmo_tier() == 0, "Graphics preset Low sets Atmosphere Off", "");
    eden_settings_set(pi, 3.0f);
    check(eden_atmo_tier() == 2 && (int)eden_settings_get(pi) == 3, "preset High sets Atmosphere High", "");
    eden_settings_set(pi, 2.0f);
    // The device default is Off on iOS Air 2-class hardware (A7), High elsewhere: an iOS run accepts either.
#if defined(EDEN_PLATFORM_IOS)
    const bool defOk = eden_atmo_tier() == 0 || eden_atmo_tier() == 2;
#else
    const bool defOk = eden_atmo_tier() == 2;
#endif
    check(defOk && (int)eden_settings_get(pi) == 2, "preset Default sets the device default (High off iOS)", "");
    eden_settings_set(ai, 1.0f);
    check((int)eden_settings_get(pi) == 0, "moving Atmosphere by hand drops the preset to Custom", "");
    eden_settings_set(pi, 2.0f);
    eden_settings_set(ai, 2.0f);
    check((int)eden_settings_get(pi) == 0 || (int)eden_settings_get(pi) == 2, "preset stays coherent after the tier returns", "");

    // SX.5: a hand-edited element row reads Custom; restoring the tier's rows snaps back.
    eden_settings_set(ai, 2.0f);
    const int sp = row("splash");
    check(sp >= 0, "splash row exists", "");
    eden_settings_set(sp, 0.0f);
    check((int)eden_settings_get(ai) == 3, "editing an element row sets Atmosphere to Custom", "");
    check(eden_atmo_tier() == 2 && eden_atmo_element(1) == 0, "Custom gates like High; the element decides", "");
    eden_settings_set(sp, 1.0f);
    check((int)eden_settings_get(ai) == 2, "restoring the rows snaps Custom back to High", "");
    eden_settings_set(ai, 0.0f);
    check(eden_atmo_element(2) == 0, "tier Off still gates every element", "");

    // atmo.* and the two user rows are console variables.
    Console::init();
    check(Console::cvarIndex("atmo.precip") >= 0 && Console::cvarIndex("atmo.halos") >= 0 && Console::cvarIndex("video.atmosphere") >= 0 &&
              Console::cvarIndex("gameplay.weather") >= 0 && Console::cvarIndex("video.reduce_flashing") >= 0,
          "atmo.*, video.atmosphere, gameplay.weather, video.reduce_flashing are cvars", "");
    eden_settings_set(ai, 2.0f);
}

// ---- AT.1b ------------------------------------------------------------------------------------------
struct Walk { int spells, changes, badSteps, stormNotFromRain; double t[Weather::KIND_COUNT]; uint64_t rng; };

// Simulates `days` in-game days at `steps` updates a day from a fresh seed.
static Walk walk(uint32_t seed, double days, int stepsPerDay) {
    Weather::resetSeed(seed);
    Walk w = {};
    const float dt = (float)(Weather::daySeconds() / stepsPerDay);
    int prev = Weather::kind();
    const long n = (long)(days * stepsPerDay);
    for (long i = 0; i < n; ++i) {
        Weather::update(dt, false);
        const int k = Weather::kind();
        w.t[k] += dt / Weather::daySeconds();
        if (k != prev) {
            w.changes++;
            if (std::abs(k - prev) != 1) w.badSteps++;
            if (k == Weather::STORM && prev != Weather::RAIN) w.stormNotFromRain++;
            prev = k;
        }
    }
    w.spells = (int)Weather::state().spells;
    w.rng = Weather::state().rng;
    return w;
}

static void schedule_leg() {
    char d[320];
    const int wi = row("weather");
    eden_weather_set_override(-1);
    eden_settings_set(wi, 1.0f);
    SkyCycle::setEnabled(false);

    // determinism: the same seed walks the same weather; another seed another
    const Walk a = walk(1234, 20, 2000), b = walk(1234, 20, 2000), c = walk(4321, 20, 2000);
    std::snprintf(d, sizeof(d), "spells %d/%d/%d changes %d rng %016llx/%016llx", a.spells, b.spells, c.spells, a.changes,
                  (unsigned long long)a.rng, (unsigned long long)c.rng);
    check(a.spells == b.spells && a.changes == b.changes && a.rng == b.rng && a.t[0] == b.t[0], "a seed walks the same weather twice", d);
    check(a.rng != c.rng, "another seed walks other weather", d);
    // the walk: one step at a time along clear - overcast - rain - storm, storm only from rain
    const Walk l = walk(99, 200, 400);
    std::snprintf(d, sizeof(d), "%d changes, %d skips, %d storms not from rain; days clear %.1f overcast %.1f rain %.1f storm %.1f",
                  l.changes, l.badSteps, l.stormNotFromRain, l.t[0], l.t[1], l.t[2], l.t[3]);
    check(l.changes > 100 && l.badSteps == 0 && l.stormNotFromRain == 0, "every change is one step; rain via overcast, storm only from rain", d);
    const Walk lg = walk(2024, 4000, 200);
    const double tot = lg.t[0] + lg.t[1] + lg.t[2] + lg.t[3];
    const double share[4] = {0.60, 0.20, 0.15, 0.05};
    bool near = true;
    for (int k = 0; k < 4; ++k) near = near && std::fabs(lg.t[k] / tot - share[k]) < 0.025;
    std::snprintf(d, sizeof(d), "4000 days: clear %.3f overcast %.3f rain %.3f storm %.3f", lg.t[0] / tot, lg.t[1] / tot, lg.t[2] / tot, lg.t[3] / tot);
    check(near, "time shares near the plan's 0.60 / 0.20 / 0.15 / 0.05", d);
    // SX.9: Storms Off never storms; Often storms more than Normal; Frequency Often rains more than Rare
    eden_settings_set(row("storms"), 0.0f);
    const Walk so = walk(2024, 1500, 200);
    eden_settings_set(row("storms"), 3.0f);
    const Walk sf = walk(2024, 1500, 200);
    eden_settings_set(row("storms"), 2.0f);
    std::snprintf(d, sizeof(d), "storm days Off %.1f Normal %.1f Often %.1f", so.t[3], lg.t[3] * 1500 / 4000, sf.t[3]);
    check(so.t[3] == 0.0 && sf.t[3] > lg.t[3] * 1500 / 4000, "Storms Off never storms; Often storms more than Normal", d);
    eden_settings_set(row("frequency"), 0.0f);
    const Walk fr = walk(2024, 1500, 200);
    eden_settings_set(row("frequency"), 2.0f);
    const Walk fo = walk(2024, 1500, 200);
    eden_settings_set(row("frequency"), 1.0f);
    std::snprintf(d, sizeof(d), "clear days Rare %.0f Often %.0f", fr.t[0], fo.t[0]);
    check(fo.t[0] < fr.t[0], "Weather frequency Often has less clear sky than Rare", d);
    float jump[4][4] = {{0, 0, 1, 1}, {0, 0, 0, 1}, {1, 0, 0, 0}, {1, 1, 0, 0}};
    Weather::setTransitions(jump);
    const Walk j = walk(3, 50, 200);
    const float def[4][4] = {{0.5f, 0.5f, 0, 0}, {0.6f, 0, 0.4f, 0}, {0, 0.4f, 0.1f, 0.5f}, {0, 0, 0.6f, 0.4f}};
    Weather::setTransitions(def);
    check(j.badSteps == 0 && j.changes == 0, "setTransitions refuses jumps (a matrix of only jumps leaves it clear)", "");
    // Day length: spell lengths are fractions of a day, so weather per in-game day does not change
    SkyCycle::setEnabled(true);
    SkyCycle::setDayLengthMinutes(10);
    const Walk s10 = walk(77, 30, 1000);
    SkyCycle::setDayLengthMinutes(60);
    const Walk s60 = walk(77, 30, 1000);
    SkyCycle::setDayLengthMinutes(20);
    SkyCycle::setEnabled(false);
    std::snprintf(d, sizeof(d), "spells over 30 days: 10-min day %d, 60-min day %d", s10.spells, s60.spells);
    check(s10.spells > 10 && std::abs(s10.spells - s60.spells) <= 1, "the same spells per in-game day at any Day length", d);
    check(Weather::daySeconds() == Weather::NO_CYCLE_DAY_SECONDS, "a day is 20 min while the cycle is off", "");

    // easing: ~45 s for a full swing, never a pop; the lock holds the spell, not the intensity
    Weather::resetSeed(5);
    Weather::set(Weather::RAIN, 1000, false);
    Weather::setLocked(true);
    for (int i = 0; i < 200; ++i) Weather::update(0.05f, false);   // 10 s
    const float i10 = Weather::intensity();
    for (int i = 0; i < 800; ++i) Weather::update(0.05f, false);   // 50 s
    std::snprintf(d, sizeof(d), "after 10 s %.4f, after 50 s %.4f, left %.1f", i10, Weather::intensity(), Weather::secondsLeft());
    check(std::fabs(i10 - 10.0f / 45.0f) < 1e-3f && Weather::intensity() == Weather::targetFor(Weather::RAIN) &&
              Weather::secondsLeft() == 1000.0f, "intensity eases at 1/45 per s to the kind's target; a locked spell never ends", d);
    Weather::setLocked(false);
    const Weather::State before = Weather::state();
    Weather::update(5.0f, true);
    const bool pausedHeld = Weather::state().intensity == before.intensity && Weather::state().secondsLeft == before.secondsLeft;
    eden_settings_set(wi, 0.0f);
    Weather::update(5.0f, false);
    const bool offHeld = !Weather::active() && Weather::state().secondsLeft == before.secondsLeft;
    eden_settings_set(wi, 1.0f);
    check(pausedHeld && offHeld, "the pause menu and Weather off both stop it (off: inactive, untouched)", "");

    // the look values: clear is exactly the identity (G1); overcast and storms darken
    Weather::resetSeed(5);
    const bool ident = Weather::lightMul() == 1.0f && Weather::skyGrey() == 0.0f && Weather::fogScale() == 1.0f &&
                       Weather::precipAmount() == 0.0f;
    Weather::set(Weather::OVERCAST, 1000, false);
    for (int i = 0; i < 60; ++i) Weather::update(1.0f, false);
    const float lmO = Weather::lightMul(), pO = Weather::precipAmount();
    Weather::set(Weather::STORM, 1000, false);
    for (int i = 0; i < 60; ++i) Weather::update(1.0f, false);
    std::snprintf(d, sizeof(d), "overcast light %.3f precip %.3f; storm light %.3f grey %.3f fog %.3f precip %.3f", lmO, pO,
                  Weather::lightMul(), Weather::skyGrey(), Weather::fogScale(), Weather::precipAmount());
    check(ident, "clear: lightMul 1, skyGrey 0, fogScale 1, precip 0 exactly", "");
    check(std::fabs(lmO - 0.7f) < 1e-4f && pO == 0.0f && std::fabs(Weather::lightMul() - 0.55f) < 1e-4f &&
              Weather::precipAmount() == 1.0f && Weather::fogScale() < 1.0f, "overcast 0.7 and dry; a storm 0.55 with full precipitation", d);

    // --weather=rain holds a full-strength spell for the run and never asks to be saved
    eden_weather_set_override(3);
    Weather::update(0.05f, false);
    const bool forced = Weather::kind() == Weather::RAIN && Weather::intensity() == Weather::targetFor(Weather::RAIN) && !Weather::needsSave();
    eden_weather_set_override(4);
    Weather::update(0.05f, false);
    const bool snow = Weather::forceSnow() && Weather::kind() == Weather::RAIN;
    eden_weather_set_override(-1);
    check(forced && snow, "--weather= forces the kind at full strength (snow = rain + the snow flag), never saved", "");
    eden_settings_set(wi, 0.0f);
}

static void region_leg() {
    char d[256];
    const int wi = row("weather");
    eden_settings_set(wi, 1.0f);
    eden_weather_set_override(-1);
    // the bed picker's cells: x = 4096*CHUNK_SIZE - GSIZE/2 + px*(GSIZE/4) + 100
    auto at = [](int p) { return (float)(4096 * CHUNK_SIZE - GSIZE / 2 + p * (GSIZE / 4) + 100); };
    int px = -1, pz = -1;
    const bool def = eden_default_region(at(0), at(1), &px, &pz);
    std::snprintf(d, sizeof(d), "default seed %d, cell %d,%d", def, px, pz);
    check(px == 0 && pz == 1, "eden_default_region maps the bed picker's cells", d);
    Weather::resetSeed(1);
    const float hi = 0.8f * T_HEIGHT, lo = 0.3f * T_HEIGHT;
    const bool none = Weather::precipAt(at(2), at(2), lo) == Weather::PRECIP_NONE;
    Weather::set(Weather::OVERCAST, 1000, false);
    for (int i = 0; i < 60; ++i) Weather::update(1.0f, false);
    const bool overcastDry = Weather::precipAt(at(2), at(2), lo) == Weather::PRECIP_NONE;
    Weather::set(Weather::RAIN, 1000, false);
    for (int i = 0; i < 60; ++i) Weather::update(1.0f, false);
    check(none && overcastDry, "nothing falls while clear or merely overcast", "");
    const int mid = Weather::precipAt(at(2), at(2), lo), top = Weather::precipAt(at(2), at(2), hi);
    std::snprintf(d, sizeof(d), "(2,2) low %d high %d (snowline %.2f)", mid, top, Weather::snowline());
    check(mid == Weather::PRECIP_RAIN && top == Weather::PRECIP_SNOW, "rain below the snowline, snow at and above it", d);
    if (def) {
        const int sm = Weather::precipAt(at(0), at(0), lo), py = Weather::precipAt(at(0), at(1), lo), oa = Weather::precipAt(at(0), at(2), hi);
        std::snprintf(d, sizeof(d), "snow mountain %d pyramid %d oasis(high) %d", sm, py, oa);
        check(sm == Weather::PRECIP_SNOW && py == Weather::PRECIP_DRY && oa == Weather::PRECIP_DRY,
              "default map: the snow mountain snows, the pyramid and the oasis stay dry (even high up)", d);
    } else {
        std::printf("[eden-weather] (not the default seed: the region cells are not checked)\n");
    }
    Console::init();
    Console::run("weather.snowline 0.2");
    const int low = Weather::precipAt(at(2), at(2), lo);
    Console::run("reset weather.snowline");
    std::snprintf(d, sizeof(d), "snowline 0.2 -> %d at %.0f; reset -> %.2f", low, lo, Weather::snowline());
    check(low == Weather::PRECIP_SNOW && Weather::snowline() == 0.75f, "the weather.snowline cvar moves the line (default 0.75)", d);
    Weather::set(Weather::RAIN, 1000, true);
    check(Weather::precipAt(at(0), at(1), lo) == Weather::PRECIP_SNOW && Weather::precipAt(at(2), at(2), lo) == Weather::PRECIP_SNOW,
          "a forced snow spell snows everywhere, dry regions too", "");
    Weather::set(Weather::RAIN, 1000, false);
    check(!Weather::forceSnow(), "the next set() clears the snow flag", "");
    eden_settings_set(wi, 0.0f);
}

static void record_leg() {
    char d[256];
    using Weather::RECORD_BYTES;
    const int wi = row("weather");
    eden_settings_set(wi, 1.0f);
    eden_weather_set_override(-1);
    Weather::resetSeed(42);
    Weather::set(Weather::STORM, 500, true);
    Weather::setLocked(true);
    for (int i = 0; i < 20; ++i) Weather::update(1.0f, false);
    uint8_t rec[RECORD_BYTES];
    Weather::encode(rec);
    const Weather::State s0 = Weather::state();
    Weather::State got = {};
    bool zeros = rec[2] == 0 && rec[3] == 0;
    for (int i = 24; i < RECORD_BYTES; ++i) zeros = zeros && rec[i] == 0;
    check(Weather::decode(rec, RECORD_BYTES, &got) && got.kind == Weather::STORM && got.flags == 3 && got.intensity == s0.intensity &&
              got.secondsLeft == 500.0f && got.rng == s0.rng && got.spells == s0.spells && zeros,
          "encode -> decode round trip (kind, locked + snow flags, intensity, seconds left, spells, RNG; reserved bytes 0)", "");
    Weather::reset();
    Weather::load(rec, RECORD_BYTES);
    const Weather::State s1 = Weather::state();
    check(s1.kind == s0.kind && s1.flags == s0.flags && s1.intensity == s0.intensity && s1.rng == s0.rng && !Weather::needsSave(),
          "load() restores the state, and a just-loaded world needs no save", "");
    uint8_t odd[RECORD_BYTES];
    std::memcpy(odd, rec, RECORD_BYTES);
    odd[1] |= 0xf0; odd[30] = 7;
    check(Weather::decode(odd, RECORD_BYTES, &got) && got.flags == 3, "unknown flag bits and reserved bytes are ignored on read", "");

    // malformed records load as none: clear, intensity 0, unseeded (so nothing to save)
    struct Bad { const char* what; int off; float f; int byte; size_t n; };
    const float nan = std::nanf(""), inf = INFINITY;
    const Bad bad[] = {{"31 bytes", -1, 0, -1, 31}, {"33 bytes", -1, 0, -1, 33}, {"kind 4", -1, 0, 4, RECORD_BYTES},
                       {"intensity NaN", 4, nan, -1, RECORD_BYTES}, {"intensity 1.5", 4, 1.5f, -1, RECORD_BYTES},
                       {"intensity -0.1", 4, -0.1f, -1, RECORD_BYTES}, {"seconds left 0", 8, 0.0f, -1, RECORD_BYTES},
                       {"seconds left inf", 8, inf, -1, RECORD_BYTES}, {"seconds left NaN", 8, nan, -1, RECORD_BYTES}};
    for (const Bad& b : bad) {
        uint8_t m[RECORD_BYTES + 1] = {};
        std::memcpy(m, rec, RECORD_BYTES);
        if (b.off >= 0) std::memcpy(m + b.off, &b.f, 4);
        if (b.byte >= 0) m[0] = (uint8_t)b.byte;
        Weather::load(rec, RECORD_BYTES);
        Weather::load(m, b.n);
        std::snprintf(d, sizeof(d), "%s: kind %d intensity %.2f", b.what, (int)Weather::kind(), Weather::intensity());
        check(!Weather::decode(m, b.n, nullptr) && Weather::kind() == Weather::CLEAR && Weather::intensity() == 0.0f &&
                  !Weather::locked() && !Weather::needsSave(), "a malformed record loads as none", d);
    }
    check(!Weather::decode(nullptr, RECORD_BYTES, nullptr), "a null record is refused", "");

    // P7: needsSave only past the coarse steps (kind/flags, >= 30 s left, >= 0.1 intensity)
    Weather::resetSeed(42);
    check(!Weather::needsSave(), "a freshly seeded world needs no save", "");
    Weather::set(Weather::CLEAR, 1000, false);
    Weather::saved();
    for (int i = 0; i < 29; ++i) Weather::update(1.0f, false);
    const bool q29 = Weather::needsSave();
    Weather::update(1.0f, false);
    const bool q30 = Weather::needsSave();
    Weather::saved();
    Weather::set(Weather::CLEAR, Weather::secondsLeft(), false);
    const bool same = Weather::needsSave();
    Weather::setLocked(true);
    const bool flag = Weather::needsSave();
    Weather::saved();
    Weather::set(Weather::OVERCAST, Weather::secondsLeft(), false);
    const bool kindChange = Weather::needsSave();
    Weather::saved();
    for (int i = 0; i < 4; ++i) Weather::update(1.0f, false);   // +0.089 intensity, locked
    const bool i4 = Weather::needsSave();
    Weather::update(1.0f, false);                                // +0.111
    const bool i5 = Weather::needsSave();
    std::snprintf(d, sizeof(d), "29 s %d, 30 s %d, same %d, lock %d, kind %d, +0.09 %d, +0.11 %d", q29, q30, same, flag, kindChange, i4, i5);
    check(!q29 && q30 && !same && flag && kindChange && !i4 && i5, "needsSave: 30 s left, a flag, the kind, 0.1 intensity; nothing smaller", d);
    Weather::reset();
    check(!Weather::needsSave(), "reset() (no record, not yet run) needs no save", "");
    eden_settings_set(wi, 0.0f);
}

static bool world_path(const char* name, char* path, size_t n) {
    path[0] = 0;
    for (int i = 0; i < eden_menu_world_count(); ++i)
        if (!std::strcmp(eden_menu_world_name(i), name)) {
            std::snprintf(path, n, "%s/%s", eden_platform_documents_root(), eden_menu_world_file(i));
            return true;
        }
    return false;
}
static bool file_weather(const char* path, std::vector<uint8_t>* out) {
    emod::EdenWorldStore st;
    if (!st.open(path, false) || !st.hasRecord(emod::T_WEATHER)) return false;
    std::vector<uint8_t> raw;
    if (st.readRecord(emod::T_WEATHER, raw) > emod::EdenWorldStore::READ_FALLBACK) return false;
    if (out) *out = raw;
    return true;
}
static long file_size(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return n;
}
static int modified_chunks() {
    Terrain* t = World::getWorld->terrain;
    int m = 0;
    for (int i = 0; i < CHUNKS_PER_SIDE * CHUNKS_PER_SIDE * CHUNKS_PER_COLUMN; ++i) if (t->chunkTable[i] && t->chunkTable[i]->modified) m++;
    return m;
}

static void save_leg(const char* name) {
    char d[1200], path[1024];
    const int wi = row("weather");
    eden_weather_set_override(-1);

    // off = absent: a world saved with Weather off has no record 12
    eden_settings_set(wi, 0.0f);
    tick(30);
    Console::init();
    Console::run("save");
    const bool off = !Weather::active() && quit_to_menu() && world_path(name, path, sizeof(path)) && !file_weather(path, nullptr);
    std::snprintf(d, sizeof(d), "%s", path);
    check(off, "Weather off: the saved world has no WEATHER record", d);

    // a pre-AT world (no record) loads clear with the setting on
    eden_settings_set(wi, 1.0f);
    const bool reopened = open_world(name, g_opt.height);
    tick(5);
    check(reopened && Weather::active() && Weather::kind() == Weather::CLEAR && Weather::intensity() == 0.0f && !Weather::needsSave(),
          "a world without the record loads clear (seeded, nothing to save yet)", "");

    // P7: weather never wakes a save. Run a while with weather on: no chunk gets modified, the file does not grow.
    Console::run("save");
    const long sz0 = file_size(path);
    const int m0 = modified_chunks();
    Weather::set(Weather::STORM, 900, false);
    tick(600);
    const long sz1 = file_size(path);
    std::snprintf(d, sizeof(d), "file %ld -> %ld B, modified chunks %d -> %d, intensity %.2f, needsSave %d", sz0, sz1, m0, modified_chunks(),
                  Weather::intensity(), Weather::needsSave());
    check(sz1 == sz0 && modified_chunks() == m0 && Weather::needsSave(), "600 frames of a storm write nothing and dirty nothing (it waits for a save)", d);

    // the next save carries it; a reload restores it
    Weather::setLocked(true);
    Console::run("save");
    const Weather::State s0 = Weather::state();
    const bool clean = !Weather::needsSave();
    std::vector<uint8_t> raw;
    const bool q = quit_to_menu();
    Weather::State f = {};
    const bool inFile = file_weather(path, &raw) && Weather::decode(raw.data(), raw.size(), &f);
    std::snprintf(d, sizeof(d), "clean %d quit %d in file %d: kind %d flags %d intensity %.3f left %.1f", clean, q, inFile, f.kind, f.flags,
                  f.intensity, f.secondsLeft);
    check(clean && inFile && f.kind == Weather::STORM && (f.flags & Weather::FLAG_LOCKED) && f.rng == s0.rng, "a save stages record 12 (32 B, codec 0)", d);
    const bool back = open_world(name, g_opt.height);
    std::snprintf(d, sizeof(d), "kind %d locked %d intensity %.3f/%.3f", (int)Weather::kind(), Weather::locked(), Weather::intensity(), s0.intensity);
    check(back && Weather::kind() == Weather::STORM && Weather::locked() && Weather::state().rng == s0.rng &&
              std::fabs(Weather::intensity() - s0.intensity) < 0.01f, "the weather round-trips through a reload", d);
    eden_settings_set(wi, 0.0f);
}

// AT.1c: the console verbs, the Lua API and the `weather` event.
struct VSink : CmdScript::Sink {
    std::string all;
    void line(int, const std::string& t) override { all += t + "\n"; }
};
static void verbs_leg() {
    char d[512];
    const int wi = row("weather");
    eden_weather_set_override(-1);
    eden_settings_set(wi, 0.0f);
    tick(2);
    Weather::resetSeed(7);
    Console::run("weather rain 100");
    check(Weather::kind() == Weather::CLEAR, "weather is refused while Gameplay > Weather is off", "");
    eden_settings_set(wi, 1.0f);
    tick(2);
    Weather::set(Weather::CLEAR, 1000, false);
    Weather::setLocked(false);
    Console::run("weather rain 100");
    check(Weather::kind() == Weather::RAIN && !Weather::forceSnow() && Weather::secondsLeft() > 98 && Weather::secondsLeft() <= 100,
          "weather rain 100 starts rain for 100 s", "");
    Console::run("weather snow");
    check(Weather::kind() == Weather::RAIN && Weather::forceSnow(), "weather snow = rain-level, snow everywhere", "");
    Console::run("weather storm bogus");
    Console::run("weather hail");
    check(Weather::kind() == Weather::RAIN, "a bad kind or seconds changes nothing", "");
    Console::run("weatherlock on");
    check(Weather::locked(), "weatherlock on locks the spell", "");
    Console::run("weatherlock maybe");
    check(Weather::locked(), "weatherlock needs on or off", "");
    Console::run("weatherlock off");
    check(!Weather::locked(), "weatherlock off", "");

    // Lua
    VSink out;
    Weather::set(Weather::CLEAR, 1000, false);
    ModScript::runConsole("return eden.weather(\"storm\", 50)", &out);
    check(Weather::kind() == Weather::STORM && out.all.find("storm") != std::string::npos, "eden.weather(kind, secs) sets and returns kind first", out.all.c_str());
    out.all.clear();
    ModScript::runConsole("eden.weatherlock(true) return eden.weather()", &out);
    check(Weather::locked() && out.all.find("true") != std::string::npos, "eden.weatherlock(true) locks; eden.weather() reads locked", out.all.c_str());
    out.all.clear();
    ModScript::runConsole("return eden.weather(\"hail\")", &out);
    check(out.all.find("invalid option") != std::string::npos, "eden.weather refuses an unknown kind", out.all.c_str());
    ModScript::runConsole("eden.weatherlock(false) seen = nil eden.on(\"weather\", function(k, p) seen = k .. \"<\" .. p end)", &out);
    Weather::set(Weather::OVERCAST, 1000, false);
    for (int i = 0; i < 3; ++i) { Weather::update(0.1f, false); ModScript::update(0.1f); }
    out.all.clear();
    ModScript::runConsole("return seen", &out);
    std::snprintf(d, sizeof(d), "seen = %s", out.all.c_str());
    check(out.all.find("overcast<") != std::string::npos, "the weather event delivers (kind, previous)", d);
    eden_settings_set(wi, 0.0f);
}

// AT.2b: rain.
static int dirty_chunks() {
    int n = 0;
    for (int i = 0; i < CHUNKS_PER_SIDE * CHUNKS_PER_SIDE * CHUNKS_PER_COLUMN; ++i) if (chunksToUpdate[i]) n++;
    return n;
}
// AT.2d: the rain bed is ambience layer 4: starts with rain at the camera, is muffled under a roof,
// and is stopped (not left decoding at volume 0) once it has been silent 3 s.
extern "C" void eden_native_audio_set_enabled(int on);
static void rain_audio_leg() {
    char d[300];
    World* w = World::getWorld;
    CocosDenshion::SimpleAudioEngine* au = CocosDenshion::SimpleAudioEngine::sharedEngine();
    const float snow0 = Weather::snowline();
    Weather::setSnowline(1.0f);
    eden_console_teleport(65536.5f, 40.0f, 65536.5f);
    // AT.11: a known start whatever the earlier legs left: a forced rain -> clear change (the intensity snaps to
    // 0) while still muted, so no stream is live when the sound comes on.
    eden_weather_set_override(3);
    tick(2);
    eden_weather_set_override(1);                  // clear
    tick(2);
    eden_native_audio_set_enabled(1);              // scripted modes mute; run with SDL_AUDIO_DRIVER=dummy
    tick(240);
    check(!au->isAmbiencePlaying(4), "clear: no rain layer", "");
    eden_weather_set_override(3);                  // rain
    tick(120);
    std::snprintf(d, sizeof(d), "playing %d", (int)au->isAmbiencePlaying(4));
    check(au->isAmbiencePlaying(4), "rain: layer 4 plays", d);
    eden_weather_set_override(1);
    tick(60 * 60);                                 // the intensity eases out (45 s), then 3 s silent
    std::snprintf(d, sizeof(d), "playing %d", (int)au->isAmbiencePlaying(4));
    check(!au->isAmbiencePlaying(4), "clear again: layer 4 stopped after 3 s silent", d);
    eden_native_audio_set_enabled(0);
    Weather::setSnowline(snow0);
    (void)w;
}

static void precip_leg() {
    char d[600];
    World* w = World::getWorld;
    const float snow0 = Weather::snowline();
    Weather::setSnowline(1.0f);                    // rain at any height here
    eden_console_teleport(65536.5f, 40.0f, 65536.5f);
    eden_weather_set_override(3);                  // rain: intensity 0.7, precipAmount 0.5
    eden_atmo_set_override(0);
    tick(60);
    std::snprintf(d, sizeof(d), "live %d drawn %d cap %d", Precip::live(), Precip::drawn(), Precip::cap());
    check(!Precip::live() && Precip::drawn() == 0 && Precip::cap() == 0, "Atmosphere Off: no rain texture or buffer (P12)", d);

    const long res0 = rnull::liveTextures() + rnull::liveBuffers();
    eden_atmo_set_override(1);
    tick(2);
    const int low = Precip::drawn();
    eden_atmo_set_override(2);
    tick(2);
    const int high = Precip::drawn();
    const long res1 = rnull::liveTextures() + rnull::liveBuffers();
    const int wantLow = (int)(Precip::CAP_LOW * Weather::precipAmount() + 0.5f), wantHigh = (int)(Precip::CAP_HIGH * Weather::precipAmount() + 0.5f);
    std::snprintf(d, sizeof(d), "Low %d (want %d), High %d (want %d), amount %.2f, R resources +%ld", low, wantLow, high, wantHigh,
                  Weather::precipAmount(), res1 - res0);
    // AT.6: the cloud layer (on at Low and High, weather or not) adds its own map + quad between res0 and res1.
    const long cloudRes = Clouds::live() ? 2 : 0;
    check(Precip::live() && low == wantLow && high == wantHigh && res1 - res0 == 2 + cloudRes, "rain draws cap x intensity streaks from one texture + one buffer", d);

    // AT.2c: splashes are High only and need atmo.splash.
    {
        const int spHigh = Precip::splashes();
        const int wantSp = (int)(Precip::SPLASH_HIGH * Weather::precipAmount() + 0.5f);
        eden_atmo_set_override(1);
        tick(2);
        const int spLow = Precip::splashes();
        eden_atmo_set_override(2);
        tick(2);
        const long resS = rnull::liveTextures() + rnull::liveBuffers();
        std::snprintf(d, sizeof(d), "High %d (want %d), Low %d, R resources %ld (rain only %ld)", spHigh, wantSp, spLow, resS, res1);
        check(spHigh == wantSp && spLow == 0 && resS == res1, "splashes: High draws SPLASH_HIGH x intensity seeds from the rain's own buffer; Low none", d);
    }

    // G3: 600 frames standing in the rain.
    tick(30);
    const long sc0 = Precip::scans(), up0 = Precip::rowUploads();
    const int dirty0 = dirty_chunks();
    int minDrawn = 1 << 30;
    for (int i = 0; i < 600; ++i) { tick(1); if (Precip::drawn() < minDrawn) minDrawn = Precip::drawn(); }
    const long res2 = rnull::liveTextures() + rnull::liveBuffers();
    std::snprintf(d, sizeof(d), "scans +%ld uploads +%ld dirty chunks %d -> %d, R resources %ld -> %ld, min drawn %d", Precip::scans() - sc0,
                  Precip::rowUploads() - up0, dirty0, dirty_chunks(), res1, res2, minDrawn);
    check(Precip::scans() == sc0 && Precip::rowUploads() == up0 && dirty_chunks() <= dirty0 && res2 == res1 && minDrawn == high,
          "G3: 600 frames of rain allocate, scan, upload and re-mesh nothing", d);

    // Shelter: a roof over the eye column, then removed.
    Vector eye, at;
    w->cam->viewPoints(&eye, &at);
    const int ex = (int)floorf(eye.x), ez = (int)floorf(eye.z), ey = (int)floorf(eye.y);
    const int t0 = Precip::top(ex, ez);
    const int roofY = ey + 6;
    const int was = w->terrain->getLand(ex, ez, roofY);
    w->terrain->setLand(ex, ez, roofY, TYPE_STONE, true);
    tick(1);
    const int t1 = Precip::top(ex, ez);
    w->terrain->setLand(ex, ez, roofY, was, true);
    tick(1);
    const int t2 = Precip::top(ex, ez);
    std::snprintf(d, sizeof(d), "eye (%d,%d,%d): top %d -> roof %d -> %d (block was %d)", ex, ez, ey, t0, t1, t2, was);
    check(t0 >= 0 && t0 <= ey && t1 == roofY + 1 && t2 == t0, "the column-top cache follows a roof placed and removed", d);

    // The box follows the camera: a 40-block move caches the new columns and drops the old ones.
    const long sc1 = Precip::scans();
    eden_console_teleport(65536.5f + 40.0f, 40.0f, 65536.5f);
    tick(3);
    w->cam->viewPoints(&eye, &at);
    const int nx = (int)floorf(eye.x), nz = (int)floorf(eye.z);
    std::snprintf(d, sizeof(d), "new (%d,%d) top %d, old (%d,%d) top %d, scans +%ld", nx, nz, Precip::top(nx, nz), ex, ez, Precip::top(ex, ez),
                  Precip::scans() - sc1);
    check(Precip::top(nx, nz) >= 0 && Precip::top(ex, ez) == -1 && Precip::scans() - sc1 >= Precip::BOX * Precip::BOX,
          "the cache follows the camera (a full box rescanned after a jump)", d);

    const long res3 = rnull::liveTextures() + rnull::liveBuffers();
    eden_atmo_set_override(0);
    Precip::render();   // as the next frame's effects pass would: frees before anything else streams in
    const long res4 = rnull::liveTextures() + rnull::liveBuffers();
    std::snprintf(d, sizeof(d), "live %d, R resources %ld -> %ld", Precip::live(), res3, res4);
    check(!Precip::live() && res3 - res4 == 2, "Atmosphere back to Off frees the rain's texture and buffer", d);
    eden_atmo_set_override(-1);
    eden_weather_set_override(-1);
    Weather::setSnowline(snow0);
}

// AT.5: mist. The fog scale (every tier) and its identities, the dawn bump, the High tier's height fog (only
// High + atmo.height_fog + mist > 0 states it), the cull past a pulled-in fog end (none while the weather is off
// or clear at noon), and G3 for 600 frames of a misty storm.
static void mist_leg() {
    char d[400];
    const bool cyc0 = SkyCycle::enabled();
    const double t0 = SkyCycle::time();
    const float snow0 = Weather::snowline();
    Weather::setSnowline(1.0f);
    eden_console_teleport(65536.5f, 40.0f, 65536.5f);
    SkyCycle::setEnabled(true);
    SkyCycle::setTime(0.5);

    eden_weather_set_override(0);                  // weather off
    tick(30);
    const long c0 = eden_mist_culled();
    tick(30);
    std::snprintf(d, sizeof(d), "fogScale %.3f mist %.3f cull %.1f culled +%ld", Weather::fogScale(), Weather::mist(),
                  Graphics::mistCullDistance(), eden_mist_culled() - c0);
    check(Weather::fogScale() == 1.0f && Weather::mist() == 0.0f && Graphics::mistCullDistance() == 0.0f && eden_mist_culled() == c0,
          "weather off: fog scale exactly 1, no mist, nothing culled (G1)", d);

    eden_weather_set_override(1);                  // clear
    tick(2);
    const float noon = Weather::fogScale(), noonMist = Weather::mist();
    SkyCycle::setTime(0.26);
    const float dawn = Weather::fogScale(), dawnMist = Weather::mist();
    SkyCycle::setTime(0.40);
    const float late = Weather::fogScale();
    SkyCycle::setEnabled(false);
    SkyCycle::setTime(0.26);
    const float nocycle = Weather::fogScale();
    SkyCycle::setEnabled(true);
    std::snprintf(d, sizeof(d), "noon %.3f (mist %.3f), dawn %.3f (mist %.3f), 0.40 %.3f, cycle off at 0.26 %.3f", noon, noonMist, dawn,
                  dawnMist, late, nocycle);
    check(noon == 1.0f && noonMist == 0.0f && std::fabs(dawn - 0.7f) < 1e-4f && dawnMist == 1.0f && late == 1.0f && nocycle == 1.0f,
          "clear: the dawn mist pulls the fog to 0.7 at sunrise only, and only with the cycle on", d);

    // Height fog: High + atmo.height_fog + mist only.
    float p[4] = {0, 0, 0, 0};
    SkyCycle::setTime(0.26);
    eden_atmo_set_override(1);
    Graphics::applyHeightFog();
    const bool lowOn = rcore::ds().hfogOn;
    eden_atmo_set_override(2);
    Graphics::applyHeightFog();
    const bool highOn = rcore::ds().hfogOn;
    for (int i = 0; i < 4; ++i) p[i] = rcore::ds().hfogParams[i];
    SkyCycle::setTime(0.5);
    Graphics::applyHeightFog();
    const bool noonOn = rcore::ds().hfogOn;
    R::heightFogOff();
    std::snprintf(d, sizeof(d), "Low %d, High at dawn %d (top %.1f strength %.2f), High at noon %d", (int)lowOn, (int)highOn, p[0], p[3], (int)noonOn);
    check(!lowOn && highOn && std::fabs(p[3] - 0.85f) < 1e-4f && std::fabs(p[0] - Weather::mistHeight() * T_HEIGHT) < 1e-3f && !noonOn,
          "height fog: stated on High with mist only (Low and a clear noon draw the plain program, P3)", d);

    // A storm pulls the fog to 0.6 and culls past it; back to clear culls nothing.
    eden_weather_set_override(5);
    tick(30);
    const long c1 = eden_mist_culled();
    tick(10);
    const long stormCulled = eden_mist_culled() - c1;
    std::snprintf(d, sizeof(d), "fogScale %.3f mist %.3f cull at %.1f, culled +%ld in 10 frames", Weather::fogScale(), Weather::mist(),
                  Graphics::mistCullDistance(), stormCulled);
    check(std::fabs(Weather::fogScale() - 0.6f) < 1e-4f && Weather::mist() == 1.0f && Graphics::mistCullDistance() > 0.0f && stormCulled > 0,
          "storm: fog x0.6, full mist, chunks past the pulled-in fog end culled (S3)", d);

    // G3: 600 frames of a misty storm at High.
    const long res0 = rnull::liveTextures() + rnull::liveBuffers();
    const int dirty0 = dirty_chunks();
    for (int i = 0; i < 600; ++i) tick(1);
    const long res1 = rnull::liveTextures() + rnull::liveBuffers();
    std::snprintf(d, sizeof(d), "dirty chunks %d -> %d, R resources %ld -> %ld", dirty0, dirty_chunks(), res0, res1);
    check(dirty_chunks() <= dirty0 && res1 == res0, "G3: 600 frames of mist allocate and re-mesh nothing", d);

    eden_weather_set_override(1);
    SkyCycle::setTime(0.5);
    tick(60 * 50);                                 // the intensity eases out (45 s)
    const long c2 = eden_mist_culled();
    tick(10);
    std::snprintf(d, sizeof(d), "fogScale %.3f culled +%ld", Weather::fogScale(), eden_mist_culled() - c2);
    check(Weather::fogScale() == 1.0f && eden_mist_culled() == c2, "clear noon again: fog scale exactly 1, nothing culled", d);

    eden_atmo_set_override(-1);
    eden_weather_set_override(-1);
    SkyCycle::setEnabled(cyc0);
    SkyCycle::setTime(t0);
    Weather::setSnowline(snow0);
}

static int g_hookCount = 0, g_hookVar[64];
static void thunder_hook(int v) { if (g_hookCount < 64) g_hookVar[g_hookCount] = v; ++g_hookCount; }

// AT.4: lightning limits (plan A9): peak <= 1.5, <= 2 flashes in any second, reduce mode never brightens,
// thunder arrives 0.5-4 s after its strike, dry = rarer + distant, snow/off = none. Drives lightningUpdate
// directly (no frames), with the weather forced to a storm so flashMul() is live.
static void lightning_leg() {
    char d[300];
    const int wi = row("weather"), ri = row("reduce_flashing");
    eden_settings_set(wi, 1.0f);
    eden_settings_set(ri, 0.0f);
    eden_weather_set_override(5);                  // storm, full strength
    tick(3);
    Weather::setThunderHook(thunder_hook);
    const float dt = 1.0f / 60.0f;
    struct Run { int strikes; float maxMul, minMul; int maxPerSec; int thunder; };
    auto sim = [&](bool eligible, bool dry, double secs) {
        Weather::lightningReset();
        Weather::setThunderHook(thunder_hook);
        g_hookCount = 0;
        Run r = {0, 1.0f, 1.0f, 0, 0};
        std::vector<double> rises;
        float prev = 1.0f;
        for (double t = 0; t < secs; t += dt) {
            Weather::lightningUpdate(dt, eligible, dry);
            const float m = Weather::flashMul();
            if (m > r.maxMul) r.maxMul = m;
            if (m < r.minMul) r.minMul = m;
            if (m > prev + 0.2f) rises.push_back(t);   // a flash begins (a strike or the second flash)
            prev = m;
        }
        for (size_t i = 0; i < rises.size(); ++i) {
            int n = 0;
            for (size_t j = i; j < rises.size() && rises[j] < rises[i] + 1.0; ++j) ++n;
            if (n > r.maxPerSec) r.maxPerSec = n;
        }
        r.strikes = Weather::strikeCount();
        r.thunder = g_hookCount;
        return r;
    };
    Run a = sim(true, false, 600);
    std::snprintf(d, sizeof(d), "%d strikes in 600 s, peak %.3f, floor %.3f, max %d flashes in 1 s", a.strikes, a.maxMul, a.minMul, a.maxPerSec);
    check(a.strikes >= 40 && a.strikes <= 140 && a.maxMul <= 1.5f + 1e-4f && a.maxMul > 1.3f && a.minMul >= 1.0f && a.maxPerSec <= 2,
          "storm: strikes every 4-12 s, peak <= 1.5x, never dims, <= 2 flashes a second", d);
    std::snprintf(d, sizeof(d), "%d thunder for %d strikes", a.thunder, a.strikes);
    check(a.thunder >= a.strikes - 1 && a.thunder <= a.strikes, "every strike gets a thunder one-shot", d);
    bool varOk = true;
    for (int i = 0; i < a.thunder && i < 64; ++i) varOk = varOk && g_hookVar[i] >= 0 && g_hookVar[i] <= 2;
    check(varOk, "thunder variations stay 0..2", "");
    // a single strike: the thunder delay lies in 0.5-4 s
    Weather::lightningReset();
    Weather::setThunderHook(thunder_hook);
    g_hookCount = 0;
    double tStrike = -1, tThunder = -1;
    for (double t = 0; t < 40 && tThunder < 0; t += dt) {
        Weather::lightningUpdate(dt, true, false);
        if (tStrike < 0 && Weather::strikeCount() == 1) tStrike = t;
        if (g_hookCount > 0) tThunder = t;
    }
    std::snprintf(d, sizeof(d), "strike at %.2f s, thunder at %.2f s (delay %.2f)", tStrike, tThunder, tThunder - tStrike);
    check(tStrike >= 0 && tThunder - tStrike >= 0.45 && tThunder - tStrike <= 4.1, "thunder follows its strike by 0.5-4 s", d);
    // Reduce flashing: only dims (0.85 floor), never above 1
    eden_settings_set(ri, 1.0f);
    Run b = sim(true, false, 300);
    std::snprintf(d, sizeof(d), "%d strikes, peak %.3f, floor %.3f", b.strikes, b.maxMul, b.minMul);
    check(b.strikes > 10 && b.maxMul <= 1.0f && b.minMul >= 0.85f - 1e-4f && b.minMul < 0.9f, "Reduce flashing: dims to 0.85 and back, never brightens", d);
    eden_settings_set(ri, 0.0f);
    // dry regions: rarer, distant thunder only; ineligible (snow / not a storm): none
    Run c = sim(true, true, 600);
    bool distant = true;
    for (int i = 0; i < c.thunder && i < 64; ++i) distant = distant && g_hookVar[i] >= 1;
    std::snprintf(d, sizeof(d), "dry %d strikes vs storm %d; all thunder >= variation 1: %d", c.strikes, a.strikes, (int)distant);
    check(c.strikes > 0 && c.strikes < a.strikes && distant, "dry region: rarer strikes, distant thunder", d);
    Run e = sim(false, false, 120);
    check(e.strikes == 0 && e.maxMul == 1.0f && e.thunder == 0, "snow / no storm: no lightning", "");
    // off = absent: flashMul is 1 with the weather off
    Weather::lightningReset();
    Weather::setThunderHook(thunder_hook);
    for (int i = 0; i < 600; ++i) Weather::lightningUpdate(dt, true, false);
    eden_weather_set_override(0);
    eden_settings_set(wi, 0.0f);
    tick(3);
    check(Weather::flashMul() == 1.0f, "weather off: flashMul is exactly 1", "");
    eden_weather_set_override(-1);
    Weather::setThunderHook(NULL);
    Weather::lightningReset();
}

// AT.6: clouds. Off = absent; Low draws the layer without shadows; High states the shadows by day only; the
// coverage is BASE_COVER with the weather off or clear and closes over in a storm; G3 for 600 frames; Off frees.
static void clouds_leg() {
    char d[400];
    const bool cyc0 = SkyCycle::enabled();
    const double t0 = SkyCycle::time();
    eden_console_teleport(65536.5f, 40.0f, 65536.5f);
    SkyCycle::setEnabled(true);
    SkyCycle::setLocked(true);
    SkyCycle::setTime(0.5);
    eden_weather_set_override(0);                  // weather off: clouds don't need it
    eden_atmo_set_override(0);
    tick(30);
    std::snprintf(d, sizeof(d), "live %d drawn %d shadow %d coverage %.2f", Clouds::live(), Clouds::drawn(), Clouds::shadowStated(), Clouds::coverage());
    check(!Clouds::live() && !Clouds::drawn() && !Clouds::shadowStated() && Clouds::coverage() == 0.0f, "Atmosphere Off: no cloud map or quad, nothing stated (P12)", d);

    const long res0 = rnull::liveTextures() + rnull::liveBuffers();
    const long up0 = Clouds::texUploads();
    eden_atmo_set_override(1);
    tick(2);
    const bool lowDrawn = Clouds::drawn(), lowShadow = Clouds::shadowStated();
    const long res1 = rnull::liveTextures() + rnull::liveBuffers();
    std::snprintf(d, sizeof(d), "drawn %d shadow %d coverage %.3f R resources +%ld uploads +%ld", lowDrawn, lowShadow, Clouds::coverage(), res1 - res0,
                  Clouds::texUploads() - up0);
    check(lowDrawn && !lowShadow && Clouds::coverage() == Clouds::BASE_COVER && res1 - res0 == 2 && Clouds::texUploads() - up0 == 1,
          "Low, weather off: the layer at the base coverage from one map + one quad, no shadows (P3)", d);

    // Cloud shadows are opt-in at High: High through the rows, with the shadow row switched on.
    eden_atmo_set_override(-1);
    eden_settings_set(row("atmosphere"), 2.0f);
    eden_settings_set(row("cloud_shadows"), 1.0f);
    tick(2);
    const bool noonShadow = Clouds::shadowStated();
    const float noonStrength = Clouds::shadowStrength();
    SkyCycle::setTime(0.0);
    tick(2);
    const bool nightShadow = Clouds::shadowStated(), nightDrawn = Clouds::drawn();
    SkyCycle::setTime(0.5);
    tick(2);
    std::snprintf(d, sizeof(d), "noon shadow %d (strength %.3f), midnight shadow %d (layer drawn %d), height %.0f", noonShadow, noonStrength, nightShadow,
                  nightDrawn, Clouds::height());
    check(noonShadow && noonStrength > 0.3f && !nightShadow && nightDrawn && Clouds::height() > T_HEIGHT,
          "High: cloud shadows by day only (the layer stays at night), the plane above the world", d);

    eden_weather_set_override(5);                  // storm
    tick(60 * 50);                                 // the intensity eases in (45 s)
    const float stormCover = Clouds::coverage();
    const bool stormShadow = Clouds::shadowStated();
    std::snprintf(d, sizeof(d), "storm coverage %.3f, shadow %d", stormCover, stormShadow);
    check(stormCover > 0.9f && !stormShadow, "storm: the sky closes over (coverage > 0.9) and the shadows fade out", d);

    // G3: 600 frames of clouds at High, clear noon (shadows on).
    eden_weather_set_override(1);
    tick(60 * 50);
    const long res2 = rnull::liveTextures() + rnull::liveBuffers(), up2 = Clouds::texUploads();
    const int dirty0 = dirty_chunks();
    int shadowFrames = 0;
    for (int i = 0; i < 600; ++i) { tick(1); if (Clouds::shadowStated() && Clouds::drawn()) shadowFrames++; }
    const long res3 = rnull::liveTextures() + rnull::liveBuffers();
    std::snprintf(d, sizeof(d), "coverage %.3f, dirty chunks %d -> %d, R resources %ld -> %ld, uploads +%ld, frames drawn with shadows %d", Clouds::coverage(), dirty0,
                  dirty_chunks(), res2, res3, Clouds::texUploads() - up2, shadowFrames);
    check(Clouds::coverage() == Clouds::BASE_COVER && dirty_chunks() <= dirty0 && res3 == res2 && Clouds::texUploads() == up2 && shadowFrames == 600,
          "G3: 600 frames of clouds + shadows allocate, upload and re-mesh nothing", d);

    const long rainRes = Precip::live() ? 2 : 0;   // the storm's rain resources go at Off too
    eden_atmo_set_override(0);
    tick(1);
    const long res4 = rnull::liveTextures() + rnull::liveBuffers();
    std::snprintf(d, sizeof(d), "live %d, R resources %ld -> %ld (rain's %ld)", Clouds::live(), res3, res4, rainRes);
    check(!Clouds::live() && res3 - res4 == 2 + rainRes && !Clouds::drawn(), "Atmosphere back to Off frees the cloud map and quad", d);

    eden_atmo_set_override(-1);
    eden_weather_set_override(-1);
    SkyCycle::setLocked(false);
    SkyCycle::setEnabled(cyc0);
    SkyCycle::setTime(t0);
}

static void ambient_leg() {
    char d[400];
    const bool cyc0 = SkyCycle::enabled();
    const double t0 = SkyCycle::time();
    eden_console_teleport(65536.5f, 40.0f, 65536.5f);
    SkyCycle::setEnabled(true);
    SkyCycle::setLocked(true);
    SkyCycle::setTime(0.5);
    eden_weather_set_override(0);
    eden_atmo_set_override(0);
    tick(30);
    std::snprintf(d, sizeof(d), "cap %d count %d drawn %d", Ambient::cap(), Ambient::count(), Ambient::drawn());
    check(Ambient::cap() == 0 && Ambient::count() == 0 && Ambient::drawn() == 0, "Atmosphere Off: no cap, an empty pool, nothing drawn (P12)", d);

    const long res0 = rnull::liveTextures() + rnull::liveBuffers();
    eden_atmo_set_override(1);
    tick(1);
    const int lowCap = Ambient::cap();
    eden_atmo_set_override(2);
    tick(1);
    const int highCap = Ambient::cap();
    std::snprintf(d, sizeof(d), "Low cap %d, High cap %d", lowCap, highCap);
    check(lowCap == Ambient::CAP_LOW && highCap == Ambient::CAP_HIGH && lowCap <= 96 && highCap <= 256, "tier caps: Low <= 96, High <= 256", d);

    // The pool never grows: force far more spawns than the cap, per tier.
    int accepted = 0;
    for (int i = 0; i < 1000; ++i)
        if (Ambient::spawnAt(Ambient::Kind(i % Ambient::KINDS), 65536.5f, 40.0f, 65536.5f)) accepted++;
    std::snprintf(d, sizeof(d), "accepted %d, count %d (cap %d)", accepted, Ambient::count(), highCap);
    check(Ambient::count() <= highCap && accepted <= highCap, "High: 1000 forced spawns never pass the cap", d);
    eden_atmo_set_override(1);
    tick(1);
    std::snprintf(d, sizeof(d), "count %d after High -> Low (cap %d)", Ambient::count(), lowCap);
    check(Ambient::count() <= lowCap, "High -> Low sheds the surplus at once", d);

    // Daylight motes appear on their own in open air, and nothing needs an R resource (G3).
    eden_atmo_set_override(2);
    Ambient::reset();
    tick(2);                                       // the clouds' map and quad (High) exist from here
    const long resBase = rnull::liveTextures() + rnull::liveBuffers();
    for (int i = 0; i < 600; ++i) tick(1);
    const int motes = Ambient::count(Ambient::MOTE), fire = Ambient::count(Ambient::FIREFLY);
    const long res1 = rnull::liveTextures() + rnull::liveBuffers();
    std::snprintf(d, sizeof(d), "motes %d fireflies %d count %d spawned %ld, R resources %ld -> %ld", motes, fire, Ambient::count(), Ambient::spawned(), resBase, res1);
    check(Ambient::count() <= highCap && fire == 0 && res1 == resBase && res0 == resBase - 2, "G3: 600 frames by day: bounded, no fireflies, no R resource of its own", d);

    // Rain and night quiet the motes; the pool still never passes the cap.
    SkyCycle::setTime(0.0);
    tick(2);
    int m0 = Ambient::count(Ambient::MOTE), mMax = m0;   // headless frames run in real time (a few ms): count, don't wait out a 16 s life
    for (int i = 0; i < 600; ++i) { tick(1); mMax = std::max(mMax, Ambient::count(Ambient::MOTE)); }
    std::snprintf(d, sizeof(d), "motes at midnight start %d, max %d, now %d, count %d", m0, mMax, Ambient::count(Ambient::MOTE), Ambient::count());
    check(mMax <= m0 && Ambient::count() <= highCap, "night: no new daylight mote spawns", d);

    eden_atmo_set_override(0);
    tick(1);
    std::snprintf(d, sizeof(d), "count %d drawn %d", Ambient::count(), Ambient::drawn());
    check(Ambient::count() == 0 && Ambient::drawn() == 0, "Atmosphere back to Off empties the pool", d);

    eden_atmo_set_override(-1);
    eden_weather_set_override(-1);
    SkyCycle::setLocked(false);
    SkyCycle::setEnabled(cyc0);
    SkyCycle::setTime(t0);
}

// AT.11: lamp halos (the redesign) and firefly glows. Three lamps floating near the eye: none by day, none on Low
// or with Atmosphere Off, three at night on High from one glow texture; 600 night frames allocate nothing; the lamp
// walk keeps the nearest when capped; Off frees the texture.
static void halos_leg() {
    char d[400];
    const bool cyc0 = SkyCycle::enabled();
    const double t0 = SkyCycle::time();
    eden_console_teleport(65536.5f, 40.0f, 65536.5f);
    eden_weather_set_override(0);
    eden_atmo_set_override(0);
    SkyCycle::setEnabled(true);
    SkyCycle::setLocked(true);
    tick(5);
    Terrain* t = World::getWorld->terrain;
    // On the ground (one above each column's top block: an empty chunk takes no cell).
    const int lx[3] = {65540, 65543, 65552}, lz = 65537;
    int ly[3], was[3];
    for (int i = 0; i < 3; ++i) {
        int y = g_world_height - 2;
        while (y > 1 && t->getLand(lx[i], lz, y) <= 0) --y;
        ly[i] = y + 1;
        was[i] = t->getLand(lx[i], lz, ly[i]);
        t->setCellState(lx[i], lz, ly[i], TYPE_LIGHTBOX, 0);
    }
    SkyCycle::setTime(0.0);
    tick(30);
    std::snprintf(d, sizeof(d), "halos %d glows %d glow texture %d", Ambient::halosDrawn(), Ambient::glowsDrawn(), Ambient::glowLive() ? 1 : 0);
    check(Ambient::halosDrawn() == 0 && !Ambient::glowLive(), "Atmosphere Off at night: no halo, no glow texture (P12)", d);

    eden_atmo_set_override(2);
    SkyCycle::setTime(0.5);
    tick(5);
    Ambient::reset();
    tick(1);
    std::snprintf(d, sizeof(d), "halos %d", Ambient::halosDrawn());
    check(Ambient::halosDrawn() == 0, "High at noon: no halos", d);

    SkyCycle::setTime(0.0);
    tick(5);
    const long up0 = Ambient::glowUploads();
    std::snprintf(d, sizeof(d), "halos %d glow texture %d", Ambient::halosDrawn(), Ambient::glowLive() ? 1 : 0);
    check(Ambient::halosDrawn() == 3 && Ambient::glowLive(), "High at midnight: a halo on each of the three lamps, one glow texture", d);

    const long res0 = rnull::liveTextures() + rnull::liveBuffers();
    int minH = 99, maxH = 0;
    for (int i = 0; i < 600; ++i) { tick(1); minH = std::min(minH, Ambient::halosDrawn()); maxH = std::max(maxH, Ambient::halosDrawn()); }
    const long res1 = rnull::liveTextures() + rnull::liveBuffers();
    std::snprintf(d, sizeof(d), "halos %d..%d, R resources %ld -> %ld, glow uploads +%ld", minH, maxH, res0, res1, Ambient::glowUploads() - up0);
    check(minH == 3 && maxH == 3 && res1 == res0 && Ambient::glowUploads() == up0, "G3: 600 night frames: steady halos, nothing allocated or uploaded", d);

    // AT.11b: a lamp the eye cannot see has no halo (it used to show through the walls round it).
    {
        const int nb[5][3] = {{lx[0] + 1, lz, ly[0]}, {lx[0] - 1, lz, ly[0]}, {lx[0], lz + 1, ly[0]},
                              {lx[0], lz - 1, ly[0]}, {lx[0], lz, ly[0] + 1}};
        int old[5], oldc[5];
        for (int i = 0; i < 5; ++i) {
            old[i] = t->getLand(nb[i][0], nb[i][1], nb[i][2]);
            oldc[i] = t->getColor(nb[i][0], nb[i][1], nb[i][2]);
            t->setCellState(nb[i][0], nb[i][1], nb[i][2], TYPE_STONE, 0);
        }
        tick(40);
        const int walled = Ambient::halosDrawn();
        for (int i = 0; i < 5; ++i) t->setCellState(nb[i][0], nb[i][1], nb[i][2], old[i] < 0 ? 0 : old[i], oldc[i] < 0 ? 0 : oldc[i]);
        tick(40);
        std::snprintf(d, sizeof(d), "halos walled in %d, open again %d", walled, Ambient::halosDrawn());
        check(walled == 2 && Ambient::halosDrawn() == 3, "a lamp boxed in by stone loses its halo; open again it comes back", d);
    }

    float pos[2][3];
    const int n = eden_lamp_positions(65536.5f, (float)ly[0], 65536.5f, 40.0f, 2, pos);
    std::snprintf(d, sizeof(d), "%d lamps: x %.1f, %.1f", n, n > 0 ? pos[0][0] : 0.0f, n > 1 ? pos[1][0] : 0.0f);
    check(n == 2 && pos[0][0] < 65550.0f && pos[1][0] < 65550.0f, "the lamp walk capped at 2 keeps the two nearest", d);

    eden_atmo_set_override(1);
    tick(2);
    std::snprintf(d, sizeof(d), "halos %d", Ambient::halosDrawn());
    check(Ambient::halosDrawn() == 0, "Low at midnight: no halos (High only)", d);

    eden_atmo_set_override(0);
    tick(2);
    std::snprintf(d, sizeof(d), "glow texture %d", Ambient::glowLive() ? 1 : 0);
    check(!Ambient::glowLive(), "Atmosphere back to Off frees the glow texture", d);

    for (int i = 0; i < 3; ++i) t->setCellState(lx[i], lz, ly[i], was[i] < 0 ? TYPE_NONE : was[i], 0);
    tick(2);
    eden_atmo_set_override(-1);
    eden_weather_set_override(-1);
    SkyCycle::setLocked(false);
    SkyCycle::setEnabled(cyc0);
    SkyCycle::setTime(t0);
}

int run_weather_selftest() {
    g_selftestTag = "eden-weather";
    const std::string leg = g_opt.sourceLeg;
    if (!leg.empty() && leg != "settings" && leg != "schedule" && leg != "region" && leg != "record" && leg != "save" && leg != "verbs" && leg != "precip" && leg != "rain-audio" && leg != "lightning" && leg != "mist" && leg != "clouds" && leg != "ambient" && leg != "halos") {
        std::fprintf(stderr, "[eden-weather] unknown leg '%s' (settings, schedule, region, record, save, verbs, precip, rain-audio, lightning, mist, clouds, ambient, halos)\n", leg.c_str());
        return 1;
    }
    const char* name = g_opt.world.empty() ? "weather-selftest" : g_opt.world.c_str();
    // AT.11b: start from a fresh world. The save leg leaves record 12 in it and saves only append, so a second
    // local run's "Weather off: no WEATHER record" failed (CI always starts clean).
    for (int i = 0, n = eden_menu_world_count(); i < n; ++i)
        if (std::strcmp(name, eden_menu_world_name(i)) == 0) { eden_menu_delete_at(i); break; }
    if (!open_world(name, g_opt.height)) return 1;
    tick(30);
    if (leg.empty() || leg == "settings") settings_leg();
    if (leg.empty() || leg == "schedule") schedule_leg();
    if (leg.empty() || leg == "region") region_leg();
    if (leg.empty() || leg == "record") record_leg();
    if (leg.empty() || leg == "save") save_leg(name);
    if (leg.empty() || leg == "verbs") verbs_leg();
    if (leg.empty() || leg == "precip") precip_leg();
    if (leg.empty() || leg == "rain-audio") rain_audio_leg();
    if (leg.empty() || leg == "lightning") lightning_leg();
    if (leg.empty() || leg == "mist") mist_leg();
    if (leg.empty() || leg == "clouds") clouds_leg();
    if (leg.empty() || leg == "ambient") ambient_leg();
    if (leg.empty() || leg == "halos") halos_leg();
    std::printf("[eden-weather] %s (%d failure(s))\n", g_selftestFailures ? "FAILURES" : "ALL PASS", g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}

}  // namespace g_weather
}  // namespace harness
