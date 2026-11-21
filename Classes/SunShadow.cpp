//  SunShadow.cpp — Stage SH.3: GPU sun shadows. See SunShadow.h.
#include <math.h>
#include <string.h>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include "SunShadow.h"
#include "Shadow.h"      // shadow_setSunReplace: the baked shadows step aside while these are on
#include "SkyCycle.h"
#include "Clouds.h"      // weatherShare: overcast fades the sun out
#include "Terrain.h"
#include "TerrainChunk.h"
#include "World.h"
#include "Camera.h"
#include "FileManager.h"
#include "Resources.h"
#include "Renderer.h"
#include "PassTimers.h"
#include "Constants.h"
#include "Globals.h"

extern TerrainChunk** chunkTablec;

namespace {
using namespace SunShadow;

#if defined(EDEN_PLATFORM_IOS)
const int MAP_N_DEFAULT = 1024;   // 2 MiB of DEPTH16, 128 blocks across
#else
const int MAP_N_DEFAULT = 2048;   // 8 MiB, 256 blocks across
#endif
int MAP_N = MAP_N_DEFAULT;        // --sunshadow-selftest shrinks it (setTestMapSize)
const double TEXELS_PER_BLOCK = 8.0;
const double BIAS_BLOCKS = 1.0 / 16.0;   // the lookup's constant bias (the depth pass adds a polygon offset)
const float FADE_BLOCKS = 8.0f;          // receivers fade to unshaded over the last 8 blocks of the distance

Tunables g_tun = {128.0f, 0.35f, 24.0f, 48.0f, 0.5f, 4.0f};
bool g_setting = false;
int  g_override = -1;
bool g_failed = false;          // the driver refused the depth framebuffer: Off for the rest of the run
bool g_replacing = false;       // what shadow_setSunReplace was last told

R::Tex g_map = 0;
bool g_dirty = true;
const char* g_why = "new";      // diagnostics: what dirtied the map last (EDEN_SUNSHADOW_TRACE=1 prints it)
bool g_haveBox = false;         // the map holds a pass for the box below
bool g_stated = false;
float g_strength = 0;
long g_renders = 0;
int g_lastCasters = 0;
std::vector<int> g_casterMins;  // diagnostics: the last pass's casters' min corners (x, y, z)

// The box the map was rendered for, in absolute world blocks (y up). Light space: a = right, b = up,
// d = toward the sun; the map covers |a|, |b| <= W and d in [dMin, dMax].
double g_C[3];                  // centre (the snapped eye, nudged to whole texels across the sun)
double g_R[3], g_U[3], g_S[3];
double g_W = 0, g_dMin = 0, g_dMax = 0, g_D = 0;
// What a re-render is keyed on.
double g_cell[3] = {0, 0, 0};    // the snap cell's centre the map was rendered around (before the texel nudge)
long long g_keySun = 0;
bool g_keyCycle = false;
int g_keyRadius = 0, g_keyHeight = 0;

double dot3(const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void freeMap() {
    if (g_map) R::texDestroy(g_map);   // frees the depth framebuffer too (it belongs to the texture)
    g_map = 0;
    g_haveBox = false;
    g_dirty = true;
}

void syncReplace() {
    const bool w = wanted();
    if (w != g_replacing) {
        g_replacing = w;
        shadow_setSunReplace(w);   // one whole-window re-mesh when the effective H.2 mode changes
    }
    if (!w) g_dirty = true;
}

// Column-major 4x4 helpers in double.
void mul(double* out, const double* a, const double* b) {   // out = a * b
    double r[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k];
            r[c * 4 + rr] = s;
        }
    memcpy(out, r, sizeof(r));
}
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

// The render-local origin (absolute x, z of render-local 0): the chunk draws subtract it.
void renderOrigin(double* ox, double* oz) {
    World* w = World::getWorld;
    *ox = (double)w->fm->chunkOffsetX * CHUNK_SIZE;
    *oz = (double)w->fm->chunkOffsetZ * CHUNK_SIZE;
}
// Light view (render-local blocks -> light space) and the light's ortho, both column-major.
void lightMatrices(double* view, double* proj) {
    double ox, oz;
    renderOrigin(&ox, &oz);
    const double c[3] = {g_C[0] - ox, g_C[1], g_C[2] - oz};
    const double* rows[3] = {g_R, g_U, g_S};
    memset(view, 0, sizeof(double) * 16);
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) view[k * 4 + r] = rows[r][k];
        view[12 + r] = -dot3(rows[r], c);
    }
    view[15] = 1;
    // glOrtho(-W, W, -W, W, n, f) with eye z = d: the near plane is d = dMax (n = -dMax), the far d = dMin.
    const double n = -g_dMax, f = -g_dMin;
    memset(proj, 0, sizeof(double) * 16);
    proj[0] = 1.0 / g_W;
    proj[5] = 1.0 / g_W;
    proj[10] = -2.0 / (f - n);
    proj[14] = -(f + n) / (f - n);
    proj[15] = 1;
}

// A chunk's absolute box against the map's, in light space.
bool overlapsBox(const int* pb) {
    double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30};
    for (int k = 0; k < 8; k++) {
        const double p[3] = {(double)((k & 1) ? pb[3] : pb[0]) - g_C[0], (double)((k & 2) ? pb[4] : pb[1]) - g_C[1],
                             (double)((k & 4) ? pb[5] : pb[2]) - g_C[2]};
        const double l[3] = {dot3(p, g_R), dot3(p, g_U), dot3(p, g_S)};
        for (int i = 0; i < 3; i++) { if (l[i] < lo[i]) lo[i] = l[i]; if (l[i] > hi[i]) hi[i] = l[i]; }
    }
    return hi[0] >= -g_W && lo[0] <= g_W && hi[1] >= -g_W && lo[1] <= g_W && hi[2] >= g_dMin && lo[2] <= g_dMax;
}

// The six face buckets' outward normals (the mesher's dx2/dy2/dz2: 0 -z, 1 +z, 2 -x, 3 +x, 4 -y, 5 +y).
const int kNormal[6][3] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}};

// Draw the selected buckets of one stream, merging neighbours into one draw.
void drawBuckets(R::Buf vb, const int* first, const int* count, const bool* sel, R::Tex alphaTex) {
    if (!vb) return;
    R::Geometry g = {};
    g.fmt = &R::kFmtTerrain; g.vbuf = vb; g.itype = R::I_NONE; g.prim = R::TRIANGLES;
    int runFirst = -1, runCount = 0;
    for (int f = 0; f <= 7; f++) {
        const bool take = f < 7 && sel[f] && count[f] > 0;
        if (take && runFirst >= 0 && runFirst + runCount == first[f]) { runCount += count[f]; continue; }
        if (runFirst >= 0 && runCount > 0) {
            g.first = runFirst; g.count = runCount;
            R::drawDepth(g, alphaTex, 0.5f);
        }
        runFirst = take ? first[f] : -1;
        runCount = take ? count[f] : 0;
    }
}

void renderPass(int N) {
    double view[16], proj[16];
    lightMatrices(view, proj);
    float vf[16], pf[16];
    for (int i = 0; i < 16; i++) { vf[i] = (float)view[i]; pf[i] = (float)proj[i]; }
    double ox, oz;
    renderOrigin(&ox, &oz);

    bool sel[7];
    for (int f = 0; f < 6; f++)
        sel[f] = kNormal[f][0] * g_S[0] + kNormal[f][1] * g_S[1] + kNormal[f][2] * g_S[2] > 0;
    sel[6] = true;   // ramps and ice: angled, some face the sun

    R::proj().push(); R::proj().load(pf);
    R::mv().push();
    R::uv().push(); R::uv().loadIdentity(); R::uv().scale(1, 1.0f / ATLAS_TILES, 1);   // atlas2's tiles, no liquid shift
    // Depth test and write, no cull (the buckets are chosen above), the slope-scaled polygon offset as the bias.
    R::setState(R::State{R::B_OFF, true, true, false, 2.0f, 4.0f});
    const bool ok = R::beginDepthPass(g_map, N);
    g_lastCasters = 0;
    g_casterMins.clear();
    if (ok) {
        const R::Tex atlas2 = Resources::getResources && Resources::getResources->atlas2 ? Resources::getResources->atlas2->name : 0;
        const int n = CHUNKS_PER_SIDE * CHUNKS_PER_SIDE * CHUNKS_PER_COLUMN;
        for (int i = 0; i < n; i++) {
            TerrainChunk* c = chunkTablec[i];
            if (!c) continue;
            const bool s1 = c->n_vertices != 0 && c->vertexBuffer;
            const bool s2 = c->n_vertices2 != 0 && c->vertexBuffer2 && atlas2;
            if (!s1 && !s2) continue;
            if (!overlapsBox(c->pbounds)) continue;
            R::mv().load(vf);
            R::mv().scale(.25f, .25f, .25f);
            R::mv().translate((float)((c->pbounds[0] - ox) * 4), (float)(c->pbounds[1] * 4), (float)((c->pbounds[2] - oz) * 4));
            if (s1) drawBuckets(c->vertexBuffer, c->face_idx, c->num_vertices, sel, 0);
            if (s2) drawBuckets(c->vertexBuffer2, c->face_idx2, c->num_vertices2, sel, atlas2);
            g_lastCasters++;
            g_casterMins.push_back(c->pbounds[0]); g_casterMins.push_back(c->pbounds[1]); g_casterMins.push_back(c->pbounds[2]);
        }
        R::endDepthPass();
        g_renders++;
        g_haveBox = true;
        g_dirty = false;
    }
    R::uv().pop();
    R::mv().pop();
    R::proj().pop();
    if (!ok) {   // the driver refused the framebuffer (logged once by the renderer): behave Off from now on
        g_failed = true;
        freeMap();
        syncReplace();
    }
}
}  // namespace

namespace SunShadow {

void setEnabled(bool on) { g_setting = on; syncReplace(); }
void setOverride(int v) { g_override = v < 0 ? -1 : v > 1 ? 1 : v; syncReplace(); }
int override() { return g_override; }
bool wanted() { return !g_failed && (g_override >= 0 ? g_override == 1 : g_setting); }

Tunables& tunables() { return g_tun; }
void tunablesChanged() { g_dirty = true; g_why = "tunable"; }

void reset() {
    freeMap();
    g_stated = false;
}

void noteChunk(const int pbounds[6]) {
    if (!g_haveBox || g_dirty) return;
    if (overlapsBox(pbounds)) { g_dirty = true; g_why = "chunk"; }
}

void renderMap() {
    g_strength = 0;
    if (!wanted()) { if (g_map) freeMap(); return; }
    World* w = World::getWorld;
    if (!w || !w->terrain || !w->fm || !w->cam || !chunkTablec) return;

    Vector sun;
    float day;
    const float step = g_tun.sunStep > 0.01f ? g_tun.sunStep : 0.01f;
    SkyCycle::shadowSun(&sun, &day, step);
    const float strength = g_tun.strength * day * (1.0f - Clouds::weatherShare());
    if (!(strength > 0.01f)) return;   // night or overcast: no pass, no lookup; the map waits for the sun
    g_strength = strength;

    // Keys: the eye against the last centre, the quantised sun, the window's shape. The eye re-renders the map
    // when it is more than a snap from the centre on any axis (not when it leaves its snap cell): hysteresis, so
    // standing, jumping or bobbing across a cell edge costs nothing, and a walk re-renders once a snap.
    Vector eye, at;
    w->cam->viewPoints(&eye, &at);
    const double snap = g_tun.snap >= 1.0f ? floor(g_tun.snap) : 1.0;
    const double ev[3] = {eye.x, eye.y, eye.z};
    bool eyeMoved = false;
    for (int i = 0; i < 3; i++) eyeMoved = eyeMoved || fabs(ev[i] - g_cell[i]) > snap;
    const bool cycle = SkyCycle::enabled();
    const long long ks = cycle ? (long long)floor(SkyCycle::time() * (360.0 / step) + 0.5) : -1;
    if (eyeMoved || ks != g_keySun || cycle != g_keyCycle ||
        g_keyRadius != T_RADIUS || g_keyHeight != T_HEIGHT) {
        if (!g_dirty) g_why = ks != g_keySun || cycle != g_keyCycle ? "sun" : "eye";
        g_dirty = true;
    }
    if (!g_map) {
        const R::TexDesc d = {MAP_N, MAP_N, R::TF_DEPTH16, false, false, true, true, "sun-shadow"};
        g_map = R::texCreate(d, nullptr);
        if (!g_map) return;
        g_dirty = true;
    }
    if (!g_dirty) return;
    static const bool trace = getenv("EDEN_SUNSHADOW_TRACE") != nullptr;
    if (trace) fprintf(stderr, "[sunshadow] render #%ld: %s (eye %.2f %.2f %.2f)\n", g_renders + 1, g_why, eye.x, eye.y, eye.z);
    g_why = "?";
    for (int i = 0; i < 3; i++) g_cell[i] = floor(ev[i] / snap) * snap + snap * 0.5;
    g_keySun = ks; g_keyCycle = cycle; g_keyRadius = T_RADIUS; g_keyHeight = T_HEIGHT;

    // The basis: forward = -sun; right = normalize(+z x forward) (the arc is in x-y, tilted toward +z, so +z is
    // never parallel to it); up = sun x right, so (right, up, sun) is a right-handed frame.
    double s[3] = {sun.x, sun.y, sun.z};
    const double sl = sqrt(dot3(s, s));
    for (int i = 0; i < 3; i++) g_S[i] = s[i] / sl;
    double r[3] = {g_S[1], -g_S[0], 0};
    const double rl = sqrt(dot3(r, r));
    for (int i = 0; i < 3; i++) g_R[i] = r[i] / rl;
    g_U[0] = g_S[1] * g_R[2] - g_S[2] * g_R[1];
    g_U[1] = g_S[2] * g_R[0] - g_S[0] * g_R[2];
    g_U[2] = g_S[0] * g_R[1] - g_S[1] * g_R[0];

    // The centre: the eye's snap cell, then moved across the sun to whole texels, so a world point keeps its
    // texel from one re-render to the next (no shimmer).
    const double* c0 = g_cell;
    const double texel = 1.0 / TEXELS_PER_BLOCK;
    const double a = dot3(c0, g_R), b = dot3(c0, g_U);
    const double da = floor(a / texel + 0.5) * texel - a, db = floor(b / texel + 0.5) * texel - b;
    for (int i = 0; i < 3; i++) g_C[i] = c0[i] + da * g_R[i] + db * g_U[i];

    // Extent: half-width W; the shaded distance D (inside the map wherever the eye is in its snap cell, the
    // window and the cvar); the depth range from the window's farthest reach toward the sun (the world's
    // ceiling, at most the window's diagonal) down to W + reach_end behind the centre.
    g_W = MAP_N / (2.0 * TEXELS_PER_BLOCK);
    double D = g_W - snap * 1.75;   // the eye is within a snap of the centre on each axis (sqrt(3) < 1.75)
    if (g_tun.distance < D) D = g_tun.distance;
    const double win = (double)T_RADIUS * CHUNK_SIZE;
    if (win < D) D = win;
    if (D < FADE_BLOCKS + 1) D = FADE_BLOCKS + 1;
    g_D = D;
    const double sy = g_S[1] > 0.05 ? g_S[1] : 0.05;
    double up = (T_HEIGHT - g_C[1] + g_W * (fabs(g_R[1]) + fabs(g_U[1]))) / sy;
    const double diag = sqrt(2.0 * (2 * win) * (2 * win) + (double)T_HEIGHT * T_HEIGHT);
    if (up > diag) up = diag;
    if (up < 1) up = 1;
    g_dMax = up;
    g_dMin = -(g_W + (g_tun.reachEnd > 0 ? g_tun.reachEnd : 0));

    pass_begin(P_R_SHADOW);
    renderPass(MAP_N);
    pass_end(P_R_SHADOW);
}

void apply() {
    g_stated = false;
    if (!wanted() || !g_map || !g_haveBox || !(g_strength > 0.01f)) { R::sunShadowOff(); return; }
    double vi[16];
    if (!invert(R::mv().top().m, vi)) { R::sunShadowOff(); return; }
    double view[16], proj[16], m[16];
    lightMatrices(view, proj);
    const double bias[16] = {0.5, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 0.5, 0, 0.5, 0.5, 0.5, 1};
    mul(m, proj, view);
    mul(m, m, vi);
    mul(m, bias, m);
    float ss[16];
    for (int i = 0; i < 16; i++) ss[i] = (float)m[i];
    const float params[4] = {g_strength, (float)(g_dMax - g_dMin), (float)BIAS_BLOCKS, 0};
    const float fade[4] = {g_tun.reachFull, g_tun.reachEnd > g_tun.reachFull ? g_tun.reachEnd : g_tun.reachFull + 0.01f,
                           (float)g_D - FADE_BLOCKS, (float)g_D};
    R::setSunShadow(g_map, ss, params, fade);
    g_stated = true;
}

bool live() { return g_map != 0; }
bool stated() { return g_stated; }
long renders() { return g_renders; }
int lastCasters() { return g_lastCasters; }
bool lastCasterHad(const int pb[6]) {
    for (size_t i = 0; i + 2 < g_casterMins.size(); i += 3)
        if (g_casterMins[i] == pb[0] && g_casterMins[i + 1] == pb[1] && g_casterMins[i + 2] == pb[2]) return true;
    return false;
}
float strengthNow() { return g_strength; }
int mapSize() { return MAP_N; }
void setTestMapSize(int n) {
    MAP_N = n > 0 ? n : MAP_N_DEFAULT;
    freeMap();
}

}
