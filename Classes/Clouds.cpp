//  Clouds.cpp — Stage AT.6: the pixel cloud layer and its shadows. See Clouds.h.
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <algorithm>
#include "Clouds.h"
#include "Weather.h"
#include "SkyCycle.h"
#include "SkyLook.h"
#include "World.h"
#include "Camera.h"
#include "FileManager.h"
#include "Renderer.h"
#include "Constants.h"
#include "Globals.h"

extern "C" int eden_atmo_tier(void);      // port/src/seam/Settings.cpp (AT.1a): 0 Off, 1 Low, 2 High
extern "C" int eden_atmo_element(int e);  // AT_CLOUDS, AT_CLOUD_SHADOWS (Weather.h)
extern float P_ZFAR;

namespace {
using namespace Clouds;

const double PERIOD = (double)TEX * CELL;   // world units per repeat
R::Tex g_tex = 0;
R::Buf g_buf = 0;
double g_windX = 0, g_windZ = 0;            // the drift so far, wrapped into [0, PERIOD)
bool g_drawn = false, g_shadow = false;
float g_shadowStrength = 0;
long g_uploads = 0;

bool layerOn() { return eden_atmo_element(AT_CLOUDS); }
bool shadowOn() { return eden_atmo_tier() >= 2 && eden_atmo_element(AT_CLOUD_SHADOWS); }

// splitmix64 of a lattice point: the map's noise, the same in every run.
float lattice(int x, int y, int period, uint64_t seed) {
    x = ((x % period) + period) % period;
    y = ((y % period) + period) % period;
    uint64_t z = seed + 0x9E3779B97F4A7C15ull * (uint64_t)(x * 4099 + y + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (float)(z >> 40) / (float)(1 << 24);
}
float smooth(float t) { return t * t * (3.0f - 2.0f * t); }
float valueNoise(float x, float y, int period, uint64_t seed) {
    const int x0 = (int)floorf(x), y0 = (int)floorf(y);
    const float fx = smooth(x - x0), fy = smooth(y - y0);
    const float a = lattice(x0, y0, period, seed), b = lattice(x0 + 1, y0, period, seed);
    const float c = lattice(x0, y0 + 1, period, seed), d = lattice(x0 + 1, y0 + 1, period, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

// The map: four octaves of tileable value noise (8, 16, 32, 64 cells across), then rank-equalised to 0..255,
// so a threshold of 1 - c leaves exactly the fraction c above it.
void buildMap(unsigned char* out) {
    const int N = TEX * TEX;
    static float v[TEX * TEX];
    static int order[TEX * TEX];
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            float s = 0, amp = 1;
            for (int o = 0; o < 4; o++) {
                const int cells = 8 << o;   // AT.11: 8..64 cells across (was 4..32): several mid-sized clouds, not one slab
                const float k = (float)cells / TEX;
                s += amp * valueNoise(x * k, y * k, cells, 0x434C4F5544ull + o);   // "CLOUD"
                amp *= 0.5f;
            }
            v[y * TEX + x] = s;
        }
    for (int i = 0; i < N; i++) order[i] = i;
    std::sort(order, order + N, [](int a, int b) { return v[a] < v[b] || (v[a] == v[b] && a < b); });
    for (int r = 0; r < N; r++) out[order[r]] = (unsigned char)((r * 256) / N);
}

void freeAll() {
    if (g_tex) R::texDestroy(g_tex);
    if (g_buf) R::bufDestroy(g_buf);
    g_tex = 0; g_buf = 0;
    g_drawn = false;
}

bool ensure() {
    if (g_tex && g_buf) return true;
    freeAll();
    static unsigned char px[TEX * TEX];
    buildMap(px);
    const R::TexDesc d = {TEX, TEX, R::TF_L8, false, false, false, false, "clouds"};
    g_tex = R::texCreate(d, px);
    g_uploads++;
    static const float quad[8] = {-1, -1, 1, -1, 1, 1, -1, 1};
    g_buf = R::bufCreate(R::BK_VERTEX, R::U_STATIC);
    R::bufData(g_buf, quad, sizeof(quad));
    return g_tex && g_buf;
}

// Weather's share of the sky: overcast closes it to ~0.78, a full storm ~0.95. BASE_COVER with it off.
void weatherCover(float* cover, float* heavy) {
    *cover = 0; *heavy = 0;
    if (!Weather::active()) return;
    const float c = 1.0f - Weather::clearSky();              // 0..1 as the intensity reaches overcast's
    float h = (Weather::skyGrey() - 0.8f * c) / 0.2f;        // 0..1 from overcast to a full storm
    *cover = c < 0 ? 0 : c > 1 ? 1 : c;
    *heavy = h < 0 ? 0 : h > 1 ? 1 : h;
}
float coverNow() {
    float c, h;
    weatherCover(&c, &h);
    return BASE_COVER + (0.78f - BASE_COVER) * c + (0.95f - 0.78f) * h;
}

// out = inverse of the column-major 4x4 m. False if singular.
bool invert(const float* mf, double* out) {
    double m[16], inv[16];
    for (int i = 0; i < 16; i++) m[i] = mf[i];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(fabs(det) > 1e-30)) return false;
    for (int i = 0; i < 16; i++) out[i] = inv[i] / det;
    return true;
}
// p = inv x (x, y, z, 1), divided by w.
void unproject(const double* inv, double x, double y, double z, double p[3]) {
    double q[4];
    for (int r = 0; r < 4; r++) q[r] = inv[r] * x + inv[4 + r] * y + inv[8 + r] * z + inv[12 + r];
    const double w = fabs(q[3]) > 1e-30 ? q[3] : 1e-30;
    for (int r = 0; r < 3; r++) p[r] = q[r] / w;
}
void rayAt(const double* inv, double x, double y, float out[3]) {
    double n[3], f[3];
    unproject(inv, x, y, -1, n);
    unproject(inv, x, y, 1, f);
    for (int i = 0; i < 3; i++) out[i] = (float)(f[i] - n[i]);
}
double wrap(double v) {
    v = fmod(v, PERIOD);
    return v < 0 ? v + PERIOD : v;
}
float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
}

namespace Clouds {

float height() { return (float)((T_HEIGHT > CLOUD_FLOOR ? T_HEIGHT : CLOUD_FLOOR) + CLOUD_ABOVE); }

void update(float etime, bool paused) {
    if (paused || !(etime > 0) || !g_tex) return;
    float c, h;
    weatherCover(&c, &h);
    const double speed = 1.2 + 2.8 * h;   // blocks a second: a storm drives them
    g_windX = wrap(g_windX + etime * speed * 0.87);
    g_windZ = wrap(g_windZ + etime * speed * 0.50);
}

void render() {
    g_drawn = false;
    const bool layer = layerOn(), shadow = shadowOn();
    if (!layer && !shadow) { if (g_tex || g_buf) freeAll(); return; }
    if (!layer) return;
    World* w = World::getWorld;
    if (!w || !w->cam) return;
    if (!ensure()) return;
    R::Mat4 vp;
    R::mat4Multiply(vp, R::proj().top(), R::mv().top());
    double inv[16];
    if (!invert(vp.m, inv)) return;
    Vector eye, at;
    w->cam->viewPoints(&eye, &at);

    R::CloudParams p;
    float r00[3], r10[3], r01[3];
    rayAt(inv, 0, 0, r00);
    rayAt(inv, 1, 0, r10);
    rayAt(inv, 0, 1, r01);
    for (int i = 0; i < 3; i++) { p.ray0[i] = r00[i]; p.ray1[i] = r10[i] - r00[i]; p.ray2[i] = r01[i] - r00[i]; }
    // The sky's ortho (-1, P_ZFAR) puts it at eye z -P_ZFAR / 1.000001 and the sun at / 1.000008: this is nearer.
    const double z = -(double)P_ZFAR / 1.000012;
    p.depth = (float)((-2.0 * z - (P_ZFAR - 1.0)) / (P_ZFAR + 1.0));
    p.eyeXZ[0] = (float)wrap((double)eye.x - g_windX);
    p.eyeXZ[1] = (float)wrap((double)eye.z - g_windZ);
    p.height = height() - eye.y;
    p.period = (float)PERIOD;
    p.threshold = 1.0f - coverNow();
    const float fe = clampf(fabsf(p.height) * 14.0f, 500.0f, 2000.0f);
    p.fade[0] = fe * 0.3f; p.fade[1] = fe;

    // AT.11: lit by the sky, not by the world light alone (which turned them muddy brown at dawn and in rain).
    // By day the clouds are the horizon colour lifted most of the way to white (peach at dawn, white at noon);
    // as the sun goes down they keep the horizon's hue, so at night they are faint silhouettes a little lighter
    // than the sky. Overcast greys them (greySky, as the sky), a storm darkens them, lightning lights them.
    float base[3] = {0.97f, 0.97f, 0.98f};
    float c, h;
    weatherCover(&c, &h);
    if (SkyCycle::enabled()) {
        float fc[4];
        SkyLook::fogColor(fc);                 // already greyed by the weather
        const float t = clampf((SkyCycle::sunElevationDeg() + 6.0f) / 30.0f, 0, 1);
        const float day = t * t * (3 - 2 * t);
        for (int i = 0; i < 3; i++) base[i] = clampf(fc[i] * (1.15f - 0.15f * day) + 0.04f + (0.97f - fc[i]) * 0.78f * day, 0, 1);
    } else {
        if (sky_isNight()) for (int i = 0; i < 3; i++) base[i] = 0.16f + 0.02f * i;
        if (Weather::active()) Weather::greySky(base);
    }
    const float dim = (1.0f - 0.38f * h) * Weather::flashMul();
    const float shade = 0.82f - 0.20f * h;
    p.lit = R::Rgba{clampf(base[0] * dim, 0, 1), clampf(base[1] * dim, 0, 1), clampf(base[2] * dim, 0, 1), 0.90f};
    p.shade = R::Rgba{p.lit.r * shade, p.lit.g * shade, p.lit.b * shade, 1.0f};
    p.tex = g_tex;

    R::setState(R::State{R::B_ALPHA, true, false, false, 0.0f, 0.0f});
    const R::Geometry g = {&R::kFmtClouds, g_buf, nullptr, 0, nullptr, R::I_NONE, 0, 4, R::TRIANGLE_FAN};
    R::drawClouds(g, p);
    g_drawn = true;
}

void applyShadow() {
    g_shadow = false;
    g_shadowStrength = 0;
    World* w = World::getWorld;
    if (!shadowOn() || !w || !w->fm) { R::cloudShadowOff(); return; }
    // The sun: the clock's, or with the cycle off a fixed high one (the painted sky has no sun), none at night.
    float day;
    Vector sun;
    if (SkyCycle::enabled()) {
        const float e = SkyCycle::sunElevationDeg();
        const float t = clampf((e - 2.0f) / 13.0f, 0, 1);
        day = t * t * (3 - 2 * t);
        sun = SkyCycle::sunDir();
    } else {
        day = sky_isNight() ? 0.0f : 1.0f;
        sun = MakeVector(0.28f, 0.94f, 0.18f);
    }
    float c, h;
    weatherCover(&c, &h);
    const float strength = 0.32f * day * (1.0f - c);
    if (!(strength > 0.01f)) { R::cloudShadowOff(); return; }
    if (!ensure()) { R::cloudShadowOff(); return; }
    double vi[16];
    if (!invert(R::mv().top().m, vi)) { R::cloudShadowOff(); return; }
    // A vertex at render-local world w = Vi e (e its eye position) lies in the shadow of the cloud where the ray
    // toward the sun meets the plane: w.xz + s (H - w.y), s = sun.xz / sun.y. In world coordinates (+ the
    // chunk offset) minus the wind, / the period: an affine function of e, with the large constant wrapped.
    const float sy = sun.y > 0.25f ? sun.y : 0.25f;
    const double sx = sun.x / sy, sz = sun.z / sy;
    const double H = height();
    const double ox = (double)w->fm->chunkOffsetX * CHUNK_SIZE, oz = (double)w->fm->chunkOffsetZ * CHUNK_SIZE;
    float u[4], v[4];
    for (int j = 0; j < 3; j++) {
        u[j] = (float)((vi[j * 4 + 0] - sx * vi[j * 4 + 1]) / PERIOD);
        v[j] = (float)((vi[j * 4 + 2] - sz * vi[j * 4 + 1]) / PERIOD);
    }
    u[3] = (float)(wrap(vi[12] + ox + sx * (H - vi[13]) - g_windX) / PERIOD);
    v[3] = (float)(wrap(vi[14] + oz + sz * (H - vi[13]) - g_windZ) / PERIOD);
    const float params[4] = {1.0f - coverNow(), strength, 0, 0};
    R::setCloudShadow(g_tex, u, v, params);
    g_shadow = true;
    g_shadowStrength = strength;
}

void reset() { freeAll(); }

bool live() { return g_tex != 0 && g_buf != 0; }
bool drawn() { return g_drawn; }
bool shadowStated() { return g_shadow; }
float coverage() { return layerOn() || shadowOn() ? coverNow() : 0.0f; }
float shadowStrength() { return g_shadowStrength; }
long texUploads() { return g_uploads; }

}
