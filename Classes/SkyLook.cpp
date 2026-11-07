//  SkyLook.cpp — Stage DN.3: the cycle's sky look (plan §5). Taste lives in kKeys (DN.8 tunes it).
//  Cost: 12-row gradient strip + a 6-vertex glow (dawn/dusk only) + <= 2 point draws (night only).
//  Stars are 400 fixed directions; they are rotated and projected on the CPU each frame and streamed
//  (<= 6.4 KB), not kept in a GPU buffer, because the camera moves.
#include <math.h>
#include <stdint.h>
#include "SkyLook.h"
#include "SkyCycle.h"
#include "World.h"
#include "Camera.h"
#include "Renderer.h"
#include "Weather.h"

extern float eden_fov_degrees;

namespace {

// Keyframes over sun elevation (degrees), shared by dawn and dusk. zen = straight up, hor = at and below
// the horizon, glow = the horizon band's tint, glowA its peak alpha.
struct Key { float s, zen[3], hor[3], glow[3], glowA; };
const Key kKeys[] = {
    {-18, {0.05f,0.08f,0.16f}, {0.07f,0.10f,0.20f}, {0.40f,0.30f,0.55f}, 0.00f},   // night
    { -8, {0.08f,0.10f,0.28f}, {0.30f,0.20f,0.40f}, {0.55f,0.30f,0.45f}, 0.35f},   // deep dusk / pre-dawn
    { -2, {0.18f,0.20f,0.45f}, {0.85f,0.45f,0.40f}, {1.00f,0.55f,0.35f}, 0.60f},   // sun just under
    {  4, {0.30f,0.42f,0.75f}, {0.95f,0.65f,0.45f}, {1.00f,0.70f,0.40f}, 0.50f},   // sun on the horizon
    { 15, {0.28f,0.50f,0.85f}, {0.75f,0.80f,0.90f}, {1.00f,0.85f,0.55f}, 0.20f},   // golden hour
    { 35, {0.22f,0.46f,0.86f}, {0.55f,0.75f,0.92f}, {1.00f,0.90f,0.70f}, 0.00f},   // morning
    { 70, {0.20f,0.44f,0.85f}, {0.50f,0.72f,0.90f}, {1.00f,0.90f,0.70f}, 0.00f},   // noon (today's fog blue)
};
const int kNumKeys = (int)(sizeof kKeys / sizeof kKeys[0]);

struct Look { float zen[3], hor[3], glow[3], glowA, starA; };

float smooth01(float x) { x = x < 0 ? 0 : (x > 1 ? 1 : x); return x * x * (3 - 2 * x); }

Look lookFor(float s) {
    Look L;
    int i = 0;
    while (i < kNumKeys - 2 && s > kKeys[i + 1].s) ++i;
    const Key &a = kKeys[i], &b = kKeys[i + 1];
    float u = (s - a.s) / (b.s - a.s);
    u = u < 0 ? 0 : (u > 1 ? 1 : u);
    for (int k = 0; k < 3; ++k) {
        L.zen[k] = a.zen[k] + (b.zen[k] - a.zen[k]) * u;
        L.hor[k] = a.hor[k] + (b.hor[k] - a.hor[k]) * u;
        L.glow[k] = a.glow[k] + (b.glow[k] - a.glow[k]) * u;
    }
    L.glowA = a.glowA + (b.glowA - a.glowA) * u;
    L.starA = smooth01((-2.0f - s) / 8.0f);   // 0 above -2 degrees, 1 below -10
    // AT.2a: overcast greys the keyframes and hides the glow and stars (identity in clear weather).
    if (Weather::active()) {
        Weather::greySky(L.zen); Weather::greySky(L.hor); Weather::greySky(L.glow);
        const float c = Weather::clearSky();
        L.glowA *= c; L.starA *= c;
    }
    return L;
}

float sunElevationDeg() {
    float y = SkyCycle::sunDir().y;
    y = y < -1 ? -1 : (y > 1 ? 1 : y);
    return asinf(y) * (float)(180.0 / M_PI);
}

struct SV { float x, y, z; uint8_t c[4]; };   // 16 bytes: position, RGBA
const R::Attr kNoAttr = {R::A_NONE, 0, 0};
const R::VertexFormat kFmtSky = {16, R::Attr{R::A_F32, 3, 0}, R::Attr{R::A_U8N, 4, 12},
                                 kNoAttr, kNoAttr, kNoAttr, kNoAttr, kNoAttr};

uint8_t b8(float v) { v = v < 0 ? 0 : (v > 1 ? 1 : v); return (uint8_t)(v * 255.0f + 0.5f); }
void setSV(SV& v, float x, float y, float z, const float rgb[3], float a) {
    v.x = x; v.y = y; v.z = z; v.c[0] = b8(rgb[0]); v.c[1] = b8(rgb[1]); v.c[2] = b8(rgb[2]); v.c[3] = b8(a);
}

const int kNumStars = 400, kBigStars = 60;   // the first kBigStars are drawn 2x
struct Star { float d[3], b, tint[3]; };
Star g_stars[kNumStars];
bool g_starsMade = false;

void makeStars() {
    uint32_t r = 0x2545F491u;   // fixed seed: the same sky every run
    auto rnd = [&r]() { r = r * 1664525u + 1013904223u; return (float)(r >> 8) / 16777216.0f; };
    for (int i = 0; i < kNumStars; ++i) {
        const float z = 2 * rnd() - 1, ph = 2 * (float)M_PI * rnd(), rr = sqrtf(1 - z * z);
        g_stars[i].d[0] = rr * cosf(ph); g_stars[i].d[1] = z; g_stars[i].d[2] = rr * sinf(ph);
        const float u = rnd();
        g_stars[i].b = i < kBigStars ? 0.75f + 0.25f * u : 0.35f + 0.5f * u * u;
        const float w = rnd();   // mostly white, a few faintly blue or yellow
        g_stars[i].tint[0] = w < 0.15f ? 0.8f : 1.0f;
        g_stars[i].tint[1] = 1.0f;
        g_stars[i].tint[2] = w < 0.15f ? 1.0f : (w > 0.85f ? 0.8f : 1.0f);
    }
    g_starsMade = true;
}

struct View { float f[3], r[3], u[3], tanHalf, aspect, pitchRad; };
View viewFor(float w, float h) {
    Camera* cam = World::getWorld->cam;
    const float sp = sinf((float)(cam->pitch * M_PI / 180.0)), cy = cosf((float)(cam->yaw * M_PI / 180.0)),
                sy = sinf((float)(cam->yaw * M_PI / 180.0)), k = 1.0f - fabsf(sp);
    View v;
    float fx = cy * k, fy = sp, fz = sy * k;   // Camera::render's look vector
    const float fl = sqrtf(fx * fx + fy * fy + fz * fz);
    fx /= fl; fy /= fl; fz /= fl;
    v.f[0] = fx; v.f[1] = fy; v.f[2] = fz;
    float rx = -fz, rz = fx;                   // right = f x (0,1,0)
    const float rl = sqrtf(rx * rx + rz * rz);
    if (rl < 1e-5f) { rx = 1; rz = 0; } else { rx /= rl; rz /= rl; }
    v.r[0] = rx; v.r[1] = 0; v.r[2] = rz;
    v.u[0] = -rz * fy; v.u[1] = rz * fx - rx * fz; v.u[2] = rx * fy;   // right x f
    const float fov = eden_fov_degrees > 0.0f ? eden_fov_degrees : 80.0f;
    v.tanHalf = tanf((float)(fov * 0.5 * M_PI / 180.0));
    v.aspect = h > 0 ? w / h : 1.0f;
    v.pitchRad = asinf(fy < -1 ? -1 : (fy > 1 ? 1 : fy));
    return v;
}

bool project(const View& v, const float d[3], float w, float h, float* sx, float* sy) {
    const float zc = d[0] * v.f[0] + d[1] * v.f[1] + d[2] * v.f[2];
    if (zc <= 0.02f) return false;
    const float xc = d[0] * v.r[0] + d[1] * v.r[1] + d[2] * v.r[2];
    const float yc = d[0] * v.u[0] + d[1] * v.u[1] + d[2] * v.u[2];
    *sx = w * (0.5f + 0.5f * xc / (zc * v.tanHalf * v.aspect));
    *sy = h * (0.5f + 0.5f * yc / (zc * v.tanHalf));
    return true;
}

// Screen y of the view elevation e (radians) for a camera looking up by v.pitchRad.
float yAt(const View& v, float e, float h) { return h * (0.5f + 0.5f * tanf(e - v.pitchRad) / v.tanHalf); }

}  // namespace

namespace SkyLook {

void fogColor(float out[4]) {
    const Look L = lookFor(sunElevationDeg());
    out[0] = L.hor[0] - 0.03f; out[1] = L.hor[1] - 0.03f; out[2] = L.hor[2] - 0.03f; out[3] = 1.0f;
}

void drawSky(float w, float h, float zfar) {
    if (!g_starsMade) makeStars();
    const Look L = lookFor(sunElevationDeg());
    const View v = viewFor(w, h);
    // Depth: the gradient where the stock sky sits, the glow and stars a hair nearer, the sun (DN.4,
    // /1.000008) nearer still, all behind any terrain.
    const float zGrad = -zfar / 1.000001f, zGlow = -zfar / 1.000004f, zStar = -zfar / 1.000006f;
    const R::State opaque = {R::B_OFF, true, true, true, 0.0f, 0.0f};
    const R::State blend = {R::B_ALPHA, true, false, true, 0.0f, 0.0f};

    {   // Horizon colour at and below the horizon, easing to the zenith colour by 60 degrees up.
        enum { ROWS = 12 };
        SV vtx[ROWS * 2];
        for (int i = 0; i < ROWS; ++i) {
            const float ndc = -1.0f + 2.0f * i / (ROWS - 1);
            const float e = v.pitchRad + atanf(ndc * v.tanHalf);
            const float t = smooth01(e / (float)(60.0 * M_PI / 180.0));
            const float y = h * i / (ROWS - 1);
            float c[3];
            for (int k = 0; k < 3; ++k) c[k] = L.hor[k] + (L.zen[k] - L.hor[k]) * t;
            setSV(vtx[2 * i], 0, y, zGrad, c, 1.0f);
            setSV(vtx[2 * i + 1], w, y, zGrad, c, 1.0f);
        }
        R::setState(opaque);
        const R::Geometry g = {&kFmtSky, 0, vtx, 0, nullptr, R::I_NONE, 0, ROWS * 2, R::TRIANGLE_STRIP};
        R::draw2D(g, 0, {1, 1, 1, 1});
    }

    if (L.glowA > 0.01f) {   // 10 degrees under to 28 above the horizon, strongest facing the sun
        const Vector sd = SkyCycle::sunDir();
        const float hl = sqrtf(sd.x * sd.x + sd.z * sd.z), fl = sqrtf(v.f[0] * v.f[0] + v.f[2] * v.f[2]);
        float facing = (hl > 1e-4f && fl > 1e-4f) ? (v.f[0] * sd.x + v.f[2] * sd.z) / (hl * fl) : 0.0f;
        facing = facing < 0 ? 0 : facing;
        const float a = L.glowA * (0.3f + 0.7f * facing * facing), rad = (float)(M_PI / 180.0);
        const float y0 = yAt(v, -10.0f * rad, h), y1 = yAt(v, 0.0f, h), y2 = yAt(v, 28.0f * rad, h);
        SV vtx[6];
        setSV(vtx[0], 0, y0, zGlow, L.glow, a);    setSV(vtx[1], w, y0, zGlow, L.glow, a);
        setSV(vtx[2], 0, y1, zGlow, L.glow, a);    setSV(vtx[3], w, y1, zGlow, L.glow, a);
        setSV(vtx[4], 0, y2, zGlow, L.glow, 0.0f); setSV(vtx[5], w, y2, zGlow, L.glow, 0.0f);
        R::setState(blend);
        const R::Geometry g = {&kFmtSky, 0, vtx, 0, nullptr, R::I_NONE, 0, 6, R::TRIANGLE_STRIP};
        R::draw2D(g, 0, {1, 1, 1, 1});
    }

    if (L.starA > 0.01f) {   // rotate about the sun's axis, project, fade near the horizon
        const double th = 2.0 * M_PI * (SkyCycle::time() - 0.25), tilt = 20.0 * M_PI / 180.0;
        const float ct = (float)cos(th), st = (float)sin(th);
        const float n[3] = {0.0f, (float)-sin(tilt), (float)cos(tilt)};   // sunDir()'s rotation axis
        SV small[kNumStars], big[kBigStars];
        int ns = 0, nb = 0;
        for (int i = 0; i < kNumStars; ++i) {
            const float* d = g_stars[i].d;
            const float nd = n[0] * d[0] + n[1] * d[1] + n[2] * d[2];
            const float cr[3] = {n[1] * d[2] - n[2] * d[1], n[2] * d[0] - n[0] * d[2], n[0] * d[1] - n[1] * d[0]};
            float q[3];
            for (int k = 0; k < 3; ++k) q[k] = d[k] * ct + cr[k] * st + n[k] * nd * (1 - ct);
            if (q[1] < 0.02f) continue;
            float sx, sy;
            if (!project(v, q, w, h, &sx, &sy) || sx < 0 || sx > w || sy < 0 || sy > h) continue;
            const float a = g_stars[i].b * L.starA * smooth01(q[1] / 0.15f);
            setSV(i < kBigStars ? big[nb++] : small[ns++], floorf(sx), floorf(sy), zStar, g_stars[i].tint, a);
        }
        int dw = 0, dh = 0;
        R::drawableSize(dw, dh);
        float unit = (dw > 0 && w > 0) ? floorf(dw / w * 1.5f + 0.5f) : 1.0f;   // ~1.5 UI points, whole pixels
        if (unit < 1.0f) unit = 1.0f;
        R::setState(blend);
        if (ns) {
            const R::Geometry g = {&kFmtSky, 0, small, 0, nullptr, R::I_NONE, 0, ns, R::POINTS};
            R::drawSprites(g, 0, R::SpriteParams{unit, {1.0f, 0.0f, 0.0f}});
        }
        if (nb) {
            const R::Geometry g = {&kFmtSky, 0, big, 0, nullptr, R::I_NONE, 0, nb, R::POINTS};
            R::drawSprites(g, 0, R::SpriteParams{unit * 2.0f, {1.0f, 0.0f, 0.0f}});
        }
    }
}

}  // namespace SkyLook
