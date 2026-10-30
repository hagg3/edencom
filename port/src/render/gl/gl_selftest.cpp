// gl_selftest.cpp — --render-selftest-gl's GL unit checks (G.5b-0; plan §9.1). On a 64x64 offscreen
// framebuffer each program draws a scene whose result at chosen pixels follows from ES 1.1's equations,
// computed here on the CPU: world (vertex colour x texel through the texture matrix, linear fog, client
// indices, with and without blending), ui (tinted, textured), sprite (point size, attenuation, fog,
// additive), skinned (two-bone palette, light 0), lit (one light with specular). Then R8 (drawLit with
// Original lighting off is drawWorld) and that consecutive draws never inherit a texture or a blend
// mode from each other (the state cache).
//
// Until G.5b-7 the world, ui and sprite checks compared R:: with the same draw through the ES 1.1 shim
// (byte-identical); the shim is gone, so the expectation is the equation (+-2/255 for mediump).
#include "gl_state.h"
#include "gl_programs.h"
#include "../render_internal.h"
#include "lit_oracle.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kW = 64, kH = 64;
std::string* g_log = nullptr;
int g_fails = 0;

void note(bool ok, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void note(bool ok, const char* fmt, ...) {
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[render-selftest-gl] %s %s\n", ok ? "PASS" : "FAIL", buf);
    if (!ok) { ++g_fails; if (g_log) { *g_log += buf; *g_log += "\n"; } }
}

typedef std::vector<unsigned char> Pixels;

Pixels grab() {
    Pixels p((size_t)kW * kH * 4);
    glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, p.data());
    return p;
}

const float kClear[4] = {0.1f, 0.2f, 0.3f, 1.0f};   // 26,51,77 in bytes
void clear() { rgl::clear(true, true, R::Rgba{kClear[0], kClear[1], kClear[2], kClear[3]}); }

const unsigned char* px(const Pixels& p, int x, int y) { return &p[((size_t)y * kW + x) * 4]; }

// One pixel against an expected colour (0..1 floats), +-tol/255 per channel (rgb; alpha too when asked).
void expect(const char* what, const Pixels& p, int x, int y, const float want[4], int tol, bool alpha) {
    const unsigned char* g = px(p, x, y);
    int worst = 0;
    for (int i = 0; i < (alpha ? 4 : 3); ++i) {
        float w = want[i] < 0.0f ? 0.0f : want[i] > 1.0f ? 1.0f : want[i];
        const int d = std::abs((int)std::lround(w * 255.0f) - (int)g[i]);
        if (d > worst) worst = d;
    }
    note(worst <= tol, "%s: (%d,%d) = %u,%u,%u,%u, want %.0f,%.0f,%.0f,%.0f (off by %d, limit %d)", what, x, y,
         g[0], g[1], g[2], g[3], want[0] * 255, want[1] * 255, want[2] * 255, want[3] * 255, worst, tol);
}
bool isClear(const unsigned char* p) { return p[0] == 26 && p[1] == 51 && p[2] == 77; }

// Two readbacks byte-identical (and not empty: a check that drew nothing proves nothing).
void same(const char* what, const Pixels& a, const Pixels& b) {
    int diff = 0, lit = 0;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) ++diff;
    for (size_t i = 0; i < a.size(); i += 4) if (!isClear(&a[i])) ++lit;
    note(diff == 0 && lit > 0, "%s: %s (%d bytes differ; %d px drawn)", what, diff ? "differ" : "identical", diff, lit);
}

// The stacks and the fog back to a known baseline between checks.
void baseline() {
    R::uv().loadIdentity();
    R::proj().loadIdentity();
    R::mv().loadIdentity();
    R::fogOff();
}
void ortho2() {   // x, y in [-2, 2] across the framebuffer; eye z = -3 for a z = 0 vertex
    R::proj().ortho(-2, 2, -2, 2, -10, 10);
    R::mv().translate(0.0f, 0.0f, -3.0f);
}

const R::State kOpaque = {R::B_OFF, true, true, false, 0.0f, 0.0f};

// A 4x4 RGBA checker with alpha, nearest, so texel choice shows.
void checkerTexel(int x, int y, float out[4]) {
    const bool on = ((x ^ y) & 1) != 0;
    out[0] = (on ? 240 : 30) / 255.0f; out[1] = (60 * x) / 255.0f; out[2] = (60 * y) / 255.0f; out[3] = (on ? 255 : 160) / 255.0f;
}
R::Tex checker() {
    unsigned char t[4 * 4 * 4];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            float c[4];
            checkerTexel(x, y, c);
            unsigned char* p = &t[(y * 4 + x) * 4];
            for (int i = 0; i < 4; ++i) p[i] = (unsigned char)std::lround(c[i] * 255.0f);
        }
    const R::TexDesc d = {4, 4, R::TF_RGBA8, false, false, false, false, "selftest-checker"};
    return R::texCreate(d, t);
}

R::Tex solid(unsigned char r, unsigned char g, unsigned char b) {
    unsigned char t[2 * 2 * 4];
    for (int i = 0; i < 4; ++i) { t[i * 4] = r; t[i * 4 + 1] = g; t[i * 4 + 2] = b; t[i * 4 + 3] = 255; }
    const R::TexDesc d = {2, 2, R::TF_RGBA8, false, false, true, true, "selftest-solid"};
    return R::texCreate(d, t);
}

const unsigned short kQuadIdx[6] = {0, 1, 2, 0, 2, 3};

// ---- world -----------------------------------------------------------------------------------------
// A quad over x, y [-1.2, 1.2] at eye z -3, uv 0..1, one vertex colour, the checker through the
// texture matrix scale (0.5, 0.25) after translate (0.25, 0.25): the pixel (32, 32) samples texel (1, 0).
// Fog 2..4.5: the eye distance is per VERTEX, as in ES 1.1, so every pixel sees the corners' sqrt(1.2^2 * 2
// + 9) = 3.447 and f = 0.421 (a per-pixel distance would give ~3 and 0.6 at the centre).
struct WV { float pos[3]; unsigned char col[4]; float uv[2]; };
void worldCheck(R::Tex tex) {
    const float fogColor[4] = {0.7f, 0.6f, 0.5f, 1.0f};
    for (int blend = 0; blend < 2; ++blend) {
        const unsigned char a = blend ? 128 : 255;
        const WV q[4] = {{{-1.2f, -1.2f, 0}, {200, 150, 100, a}, {0, 0}}, {{1.2f, -1.2f, 0}, {200, 150, 100, a}, {1, 0}},
                         {{1.2f, 1.2f, 0}, {200, 150, 100, a}, {1, 1}}, {{-1.2f, 1.2f, 0}, {200, 150, 100, a}, {0, 1}}};
        static const R::VertexFormat kFmtWV = {sizeof(WV), {R::A_F32, 3, 0}, {R::A_U8N, 4, 12}, {R::A_F32, 2, 16},
                                               {R::A_NONE, 0, 0}, {R::A_NONE, 0, 0}, {R::A_NONE, 0, 0}, {R::A_NONE, 0, 0}};
        baseline(); clear(); ortho2();
        R::uv().scale(0.5f, 0.25f, 1.0f); R::uv().translate(0.25f, 0.25f, 0.0f);
        R::State st = kOpaque;
        if (blend) st.blend = R::B_ALPHA;
        R::setState(st);
        R::setFog({fogColor[0], fogColor[1], fogColor[2], fogColor[3]}, 2.0f, 4.5f);
        const R::Geometry g = {&kFmtWV, 0, q, 0, kQuadIdx, R::I_U16, 0, 6, R::TRIANGLES};
        R::drawWorld(g, tex, {1, 1, 1, 1}, -1.0f);
        const Pixels p = grab();
        float t[4], want[4];
        checkerTexel(1, 0, t);
        const float c[4] = {200 / 255.0f, 150 / 255.0f, 100 / 255.0f, a / 255.0f};
        const float d = std::sqrt(1.2f * 1.2f * 2 + 9.0f), f = (4.5f - d) / 2.5f;
        for (int i = 0; i < 3; ++i) want[i] = fogColor[i] * (1 - f) + c[i] * t[i] * f;
        want[3] = c[3] * t[3];
        if (blend) for (int i = 0; i < 3; ++i) want[i] = want[i] * want[3] + kClear[i] * (1 - want[3]);
        expect(blend ? "world (texmat, fog, client indices, blend)" : "world (texmat, fog, client indices)", p, 32, 32,
               want, 2, false);
        note(isClear(px(p, 2, 2)) && isClear(px(p, 12, 12)) && !isClear(px(p, 14, 14)) && !isClear(px(p, 50, 50)) &&
             isClear(px(p, 52, 52)), "world: the quad covers x, y [-1.2, 1.2] (px 12.8..51.2) and no more");
    }
}

// ---- ui --------------------------------------------------------------------------------------------
void uiCheck(R::Tex tex) {
    struct UV { float pos[2]; float uv[2]; };
    const UV q[4] = {{{6, 6}, {0, 0}}, {{58, 6}, {1, 0}}, {{6, 40}, {0, 1}}, {{58, 40}, {1, 1}}};
    const float tint[4] = {0.9f, 0.5f, 0.25f, 1.0f};
    for (int textured = 0; textured < 2; ++textured) {
        baseline(); clear();
        R::proj().ortho(0, kW, 0, kH, -10, 10);
        R::State st = kOpaque;
        st.depthTest = false;
        R::setState(st);
        static const R::VertexFormat kPosOnly = {sizeof(UV), {R::A_F32, 2, 0}, {R::A_NONE, 0, 0}, {R::A_NONE, 0, 0},
                                                 {R::A_NONE, 0, 0}, {R::A_NONE, 0, 0}, {R::A_NONE, 0, 0}, {R::A_NONE, 0, 0}};
        const R::Geometry g = {textured ? &R::kFmtUI2UV : &kPosOnly, 0, q, 0, nullptr, R::I_NONE, 0, 4, R::TRIANGLE_STRIP};
        R::draw2D(g, textured ? tex : 0, {tint[0], tint[1], tint[2], tint[3]});
        const Pixels p = grab();
        float want[4] = {tint[0], tint[1], tint[2], tint[3]};
        if (textured) {   // (32, 23) samples texel (2, 2)
            float t[4];
            checkerTexel(2, 2, t);
            for (int i = 0; i < 4; ++i) want[i] *= t[i];
        }
        expect(textured ? "ui (textured, tint)" : "ui (untextured, tint)", p, 32, 23, want, 1, true);
        note(isClear(px(p, 3, 3)) && isClear(px(p, 32, 44)), "ui: the strip covers (6..58, 6..40) and no more");
    }
}

// ---- sprites ---------------------------------------------------------------------------------------
// One 30 px point at the centre, eye distance 3, attenuation (1, 0, 0), a solid green texture, colour
// (255, 128, 64), fog 2.5..4 (f = 2/3), additive over the clear colour.
void spriteCheck() {
    struct SV { float pos[3]; unsigned char col[4]; float size; };
    const SV pt[1] = {{{0.0f, 0.0f, 0.0f}, {255, 128, 64, 255}, 30.0f}};
    const float fogColor[4] = {0.2f, 0.3f, 0.9f, 1.0f};
    const R::Tex green = solid(0, 255, 0);
    baseline(); clear(); ortho2();
    R::State st = kOpaque;
    st.blend = R::B_ADD;
    R::setState(st);
    R::setFog({fogColor[0], fogColor[1], fogColor[2], fogColor[3]}, 2.5f, 4.0f);
    const R::VertexFormat fmt = {sizeof(SV), {R::A_F32, 3, 0}, {R::A_U8N, 4, 12}, {R::A_NONE, 0, 0},
                                 {R::A_NONE, 0, 0}, {R::A_F32, 1, 16}, {R::A_NONE, 0, 0}, {R::A_NONE, 0, 0}};
    const R::Geometry g = {&fmt, 0, pt, 0, nullptr, R::I_NONE, 0, 1, R::POINTS};
    const R::SpriteParams sp = {1.0f, {1.0f, 0.0f, 0.0f}};
    R::drawSprites(g, green, sp);
    const Pixels p = grab();
    const float c[3] = {0.0f, 128 / 255.0f, 0.0f}, f = (4.0f - 3.0f) / 1.5f;
    float want[4] = {0, 0, 0, 1};
    for (int i = 0; i < 3; ++i) want[i] = kClear[i] + fogColor[i] * (1 - f) + c[i] * f;
    expect("sprite (texture, fog, additive)", p, 32, 32, want, 2, false);
    note(!isClear(px(p, 44, 32)) && isClear(px(p, 50, 32)), "sprite: 30 px across (x=44 inside, x=50 outside)");
    R::texDestroy(green);
}

// ---- skinned ---------------------------------------------------------------------------------------
// Against ES 1.1 on the CPU (G.5b-5 deleted the shim's matrix-palette program it was compared with): two bones half-weighted, a point light at the eye. The
// scene-shot A/B against the shim (G.5b-5 results) proved the creatures' pixels unchanged.
void skinnedCheck(R::Tex /*tex*/) {
    struct KV { float pos[3]; float n[3]; float uv[2]; unsigned char idx[2]; unsigned char pad[2]; float w[2]; };
    const KV v[4] = {
        {{-1, -1, 0}, {0, 0, 1}, {0, 0}, {0, 1}, {0, 0}, {0.5f, 0.5f}},
        {{ 1, -1, 0}, {0, 0, 1}, {1, 0}, {0, 1}, {0, 0}, {0.5f, 0.5f}},
        {{ 1,  1, 0}, {0, 0, 1}, {1, 1}, {1, 0}, {0, 0}, {0.5f, 0.5f}},
        {{-1,  1, 0}, {0, 0, 1}, {0, 1}, {1, 0}, {0, 0}, {0.5f, 0.5f}},
    };
    // Palette entries map object space straight to eye space (Model.cpp's convention). Bone 0 alone
    // would put the quad at x [-1.5, 0.5], bone 1 alone at [-0.5, 1.5]; the blend at [-1, 1].
    R::MatrixStack m0, m1;
    m0.translate(-0.5f, 0.0f, -3.0f);
    m1.translate(0.5f, 0.0f, -3.0f);
    float palette[16 * 2];
    std::memcpy(&palette[0], m0.top().m, 64);
    std::memcpy(&palette[16], m1.top().m, 64);
    const float amb[4] = {0.2f, 0.15f, 0.1f, 1.0f}, diff[4] = {0.7f, 0.8f, 0.6f, 1.0f}, model[4] = {0.2f, 0.2f, 0.2f, 1.0f};
    const float tint[3] = {0.9f, 0.8f, 0.7f};

    baseline(); clear();
    R::proj().ortho(-2, 2, -2, 2, -10, 10);
    R::setState(kOpaque);
    R::fogOff();
    R::LightSet ls = {};
    ls.light[0].on = true;
    ls.light[0].pos[3] = 1.0f;   // (0,0,0,1): at the eye
    std::memcpy(ls.light[0].ambient, amb, 16); std::memcpy(ls.light[0].diffuse, diff, 16);
    std::memcpy(ls.modelAmbient, model, 16);
    R::setLights(ls);
    const R::VertexFormat fmt = {sizeof(KV), {R::A_F32, 3, 0}, {R::A_NONE, 0, 0}, {R::A_F32, 2, 24},
                                 {R::A_F32, 3, 12}, {R::A_NONE, 0, 0}, {R::A_U8, 2, 32}, {R::A_F32, 2, 36}};
    R::Geometry g = {&fmt, 0, v, 0, kQuadIdx, R::I_U16, 0, 6, R::TRIANGLES};
    R::drawSkinned(g, 0, {tint[0], tint[1], tint[2], 1.0f}, palette, 2, 2);
    const Pixels a = grab();
    // ES 1.1 lights per vertex: every corner is at (+-1, +-1, -3) in eye space, so n.L = 3/sqrt(11)
    // at all four and the centre is c * (model + amb0) + c * dif0 * 3/sqrt(11).
    const float ndotl = 3.0f / std::sqrt(11.0f);
    int worst = 0;
    for (int i = 0; i < 3; ++i) {
        float e = tint[i] * (model[i] + amb[i]) + tint[i] * diff[i] * ndotl;
        if (e > 1.0f) e = 1.0f;
        const int want = (int)std::lround(e * 255.0f);
        const int got = px(a, kW / 2, kH / 2)[i];
        if (std::abs(want - got) > worst) worst = std::abs(want - got);
    }
    note(worst <= 1, "skinned vs ES 1.1 on the CPU: centre pixel within %d/255 (limit 1)", worst);
    // The blend's extent: x = +0.8 is inside only if both bones count; x = -1.3 is outside only then.
    const int xin = (int)((0.8f + 2.0f) / 4.0f * kW), xout = (int)((-1.3f + 2.0f) / 4.0f * kW);
    const unsigned char* bg = px(a, 0, 0);
    const unsigned char* pin = px(a, xin, kH / 2);
    const unsigned char* pout = px(a, xout, kH / 2);
    note(std::memcmp(pin, px(a, kW / 2, kH / 2), 4) == 0 && std::memcmp(pout, bg, 4) == 0,
         "skinned: two-bone blend puts the quad at x [-1, 1] (x=+0.8 %u,%u,%u; x=-1.3 %u,%u,%u)",
         pin[0], pin[1], pin[2], pout[0], pout[1], pout[2]);
}

// ---- lit vs ES 1.1 on the CPU, and R8 ---------------------------------------------------------------
void litCheck(R::Tex tex) {
    struct LV { float pos[3]; float n[3]; unsigned char col[4]; };
    const LV v[4] = {{{-1, -1, 0}, {0, 0, 1}, {128, 102, 77, 255}}, {{1, -1, 0}, {0, 0, 1}, {128, 102, 77, 255}},
                     {{1, 1, 0}, {0, 0, 1}, {128, 102, 77, 255}}, {{-1, 1, 0}, {0, 0, 1}, {128, 102, 77, 255}}};
    const R::VertexFormat fmt = {sizeof(LV), {R::A_F32, 3, 0}, {R::A_U8N, 4, 24}, {R::A_NONE, 0, 0},
                                 {R::A_F32, 3, 12}, {R::A_NONE, 0, 0}, {R::A_NONE, 0, 0}, {R::A_NONE, 0, 0}};
    R::Geometry g = {&fmt, 0, v, 0, kQuadIdx, R::I_U16, 0, 6, R::TRIANGLES};
    baseline(); clear();
    R::proj().ortho(-2, 2, -2, 2, -10, 10);
    R::setState(kOpaque);
    R::LightSet ls = {};
    ls.light[0].on = true;
    const float pos[4] = {0, 0, 1, 0}, amb[4] = {0.1f, 0.1f, 0.1f, 1}, dif[4] = {0.6f, 0.6f, 0.6f, 1}, spc[4] = {0.5f, 0.5f, 0.5f, 1};
    std::memcpy(ls.light[0].pos, pos, 16); std::memcpy(ls.light[0].ambient, amb, 16);
    std::memcpy(ls.light[0].diffuse, dif, 16); std::memcpy(ls.light[0].specular, spc, 16);
    const float model[4] = {0.2f, 0.2f, 0.2f, 1}, mspec[4] = {1, 1, 1, 1};
    std::memcpy(ls.modelAmbient, model, 16); std::memcpy(ls.matSpecular, mspec, 16);
    ls.shininess = 8.0f;
    R::setLights(ls);
    R::drawLit(g, 0, {1, 1, 1, 1});
    const Pixels a = grab();
    // ES 1.1: c * (model + amb0) + c * dif0 * (n.L) + spec0 * matSpec * (n.h)^s, n = L = h = +z.
    const float c[3] = {128 / 255.0f, 102 / 255.0f, 77 / 255.0f};
    int worst = 0;
    for (int i = 0; i < 3; ++i) {
        float e = c[i] * (0.2f + 0.1f) + c[i] * 0.6f + 0.5f;
        if (e > 1.0f) e = 1.0f;
        const int want = (int)std::lround(e * 255.0f);
        const int got = px(a, kW / 2, kH / 2)[i];
        if (std::abs(want - got) > worst) worst = std::abs(want - got);
    }
    note(worst <= 1, "lit vs ES 1.1 on the CPU: centre pixel within %d/255 (limit 1)", worst);

    // R8: with Original lighting off, drawLit is drawWorld.
    R::setOriginalLighting(false);
    clear();
    R::drawLit(g, tex, {1, 1, 1, 1});
    const Pixels off = grab();
    clear();
    R::drawWorld(g, tex, {1, 1, 1, 1}, -1.0f);
    const Pixels world = grab();
    R::setOriginalLighting(true);
    same("drawLit with Original lighting off == drawWorld (R8)", world, off);
}

// ---- the state cache -------------------------------------------------------------------------------
// Each draw states its own texture and blend: a half-alpha red draw with blending, then a green draw with
// it off, must give a blended red left half and a pure green right half. Then a texture deleted while
// bound and a new one (GL may recycle the name) must not leave the cache claiming the old binding.
void cacheCheck() {
    const R::Tex red = solid(255, 0, 0), green = solid(0, 255, 0);
    baseline(); clear();
    R::proj().ortho(0, kW, 0, kH, -10, 10);
    struct UV { float pos[2]; float uv[2]; };
    const UV left[4] = {{{0, 0}, {0, 0}}, {{32, 0}, {1, 0}}, {{0, 64}, {0, 1}}, {{32, 64}, {1, 1}}};
    const UV right[4] = {{{32, 0}, {0, 0}}, {{64, 0}, {1, 0}}, {{32, 64}, {0, 1}}, {{64, 64}, {1, 1}}};
    R::State st = kOpaque;
    st.depthTest = false;
    st.blend = R::B_ALPHA;
    R::setState(st);
    R::draw2D({&R::kFmtUI2UV, 0, left, 0, nullptr, R::I_NONE, 0, 4, R::TRIANGLE_STRIP}, red, {1, 1, 1, 0.5f});
    st.blend = R::B_OFF;
    R::setState(st);
    R::draw2D({&R::kFmtUI2UV, 0, right, 0, nullptr, R::I_NONE, 0, 4, R::TRIANGLE_STRIP}, green, {1, 1, 1, 1});
    Pixels p = grab();
    const unsigned char* l = px(p, 16, 32);
    const unsigned char* r = px(p, 48, 32);
    // 0.5 * red + 0.5 * clear (26,51,77) = (140.5, 25.5, 38.5): +-1 for the rounding.
    note(std::abs(l[0] - 140) <= 1 && std::abs(l[1] - 25) <= 1 && std::abs(l[2] - 38) <= 1,
         "cache: the blended draw got its own texture and blend (left %u,%u,%u, want ~140,25,38)", l[0], l[1], l[2]);
    note(r[0] == 0 && r[1] == 255 && r[2] == 0, "cache: the next draw got its own texture, blend off (right %u,%u,%u)",
         r[0], r[1], r[2]);
    R::texDestroy(red);
    const R::Tex blue = solid(0, 0, 255);
    clear();
    R::draw2D({&R::kFmtUI2UV, 0, left, 0, nullptr, R::I_NONE, 0, 4, R::TRIANGLE_STRIP}, blue, {1, 1, 1, 1});
    p = grab();
    l = px(p, 16, 32);
    note(l[0] == 0 && l[1] == 0 && l[2] == 255, "cache: a texture created after deleting the bound one draws (%u,%u,%u)",
         l[0], l[1], l[2]);
    R::texDestroy(green);
    R::texDestroy(blue);
}

}  // namespace

// Runs every check on a private framebuffer and puts the engine's framebuffer, viewport and clear colour
// back. Returns the number of failures; their lines are appended to *log.
namespace rgl {
int selftest(std::string* log) {
    g_log = log;
    g_fails = 0;
    std::string clog;
    const int linked = rgl::compileAll(&clog);
    note(linked == rgl::P_PROG_COUNT, "the programs compile and link (%d/9: the five + lit-flat + three discard-free twins)%s%s", linked,
         clog.empty() ? "" : ": ", clog.c_str());
    GLint prevFbo = 0, prevRb = 0, vp[4];
    GLfloat cc[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &prevRb);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    GLuint fbo = 0, rb[2] = {0, 0};
    glGenFramebuffers(1, &fbo);
    glGenRenderbuffers(2, rb);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kW, kH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kW, kH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    note(complete, "the 64x64 check framebuffer is complete");
    if (complete) {
        glViewport(0, 0, kW, kH);
        R::beginFrame();
        const R::Tex tex = checker();
        worldCheck(tex);
        uiCheck(tex);
        spriteCheck();
        skinnedCheck(tex);
        litCheck(tex);
        cacheCheck();
        R::texDestroy(tex);
        baseline();
        R::endFrame();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)prevRb);
    glDeleteRenderbuffers(2, rb);
    glDeleteFramebuffers(1, &fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    g_log = nullptr;
    return g_fails;
}
}  // namespace rgl

// ---- --lit-oracle (G.5b-4; plan §7) ----------------------------------------------------------------
// The same scenes drawn through real fixed-function GL (lit_oracle_legacy_macos.cpp, a CGL legacy 2.1
// context) and through R::drawLit here; the two must agree within 2/255 on every pixel both cover whose
// four neighbours both cover too (an edge pixel may round to the other side of a triangle edge).
#if !defined(EDEN_LIT_ORACLE_LEGACY)
extern "C" int eden_lit_oracle_legacy(const EdenLitScene*, int, int, int, unsigned char*) { return 0; }
#endif

namespace {

constexpr int kOW = 128, kOH = 128;

// A cube tessellated 4x4 per face, normals pointing away from the centre (like the debris tetrahedra's
// vertex normals: smooth and flat shading then differ), a colour per vertex.
void oracleMesh(std::vector<EdenLitVertex>& v, std::vector<unsigned short>& idx) {
    const int N = 4;
    for (int f = 0; f < 6; ++f) {
        const int ax = f % 3; const float sgn = f < 3 ? 1.0f : -1.0f;
        const int u = (ax + 1) % 3, w = (ax + 2) % 3;
        const int base = (int)v.size();
        for (int j = 0; j <= N; ++j)
            for (int i = 0; i <= N; ++i) {
                EdenLitVertex e = {};
                e.pos[ax] = sgn; e.pos[u] = -1.0f + 2.0f * i / N; e.pos[w] = (-1.0f + 2.0f * j / N) * sgn;
                const float l = std::sqrt(e.pos[0] * e.pos[0] + e.pos[1] * e.pos[1] + e.pos[2] * e.pos[2]);
                for (int c = 0; c < 3; ++c) e.normal[c] = e.pos[c] / l;
                e.rgba[0] = (unsigned char)(150 + 25 * i); e.rgba[1] = (unsigned char)(120 + 30 * j);
                e.rgba[2] = (unsigned char)(90 + 20 * f); e.rgba[3] = 255;
                e.uv[0] = 2.0f * i / N; e.uv[1] = 2.0f * j / N;
                v.push_back(e);
            }
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i) {
                const unsigned short a = (unsigned short)(base + j * (N + 1) + i), b = (unsigned short)(a + 1),
                                     c = (unsigned short)(a + N + 1), d = (unsigned short)(c + 1);
                const unsigned short t[6] = {a, b, d, a, d, c};
                idx.insert(idx.end(), t, t + 6);
            }
    }
}

void set4(float* d, float a, float b, float c, float e) { d[0] = a; d[1] = b; d[2] = c; d[3] = e; }

// The engine's lit objects (plan §2.2) plus the debris' far light (G.5b-4) and a directional light.
std::vector<EdenLitScene> oracleScenes(const std::vector<EdenLitVertex>& v, const std::vector<unsigned short>& idx,
                                       const unsigned char* tex) {
    R::MatrixStack p, m;
    p.frustum(-0.1f, 0.1f, -0.1f, 0.1f, 0.25f, 50.0f);
    m.translate(0.0f, 0.0f, -4.5f);
    m.rotate(35.0f, 1.0f, 0.3f, 0.0f);
    m.rotate(25.0f, 0.0f, 1.0f, 0.0f);
    EdenLitScene b = {};
    b.v = v.data(); b.nv = (int)v.size(); b.idx = idx.data(); b.ni = (int)idx.size();
    std::memcpy(b.proj, p.top().m, sizeof b.proj); std::memcpy(b.mv, m.top().m, sizeof b.mv);
    b.uvScale[0] = b.uvScale[1] = 1.0f;
    b.vertexColor = 1; set4(b.tint, 1, 1, 1, 1);
    b.lightOn[0] = 1;
    set4(b.lpos[0], 0, 0, 0, 1); set4(b.lamb[0], .3f, .3f, .3f, 1); set4(b.ldiff[0], .7f, .7f, .7f, 1);
    set4(b.lspec[0], 1, 1, 1, 1);   // LIGHT0's default; no effect while the material specular is 0
    set4(b.modelAmbient, .2f, .2f, .2f, 1); set4(b.matSpec, 0, 0, 0, 1); b.shininess = 0;
    b.tex = tex; b.tw = 4; b.th = 4;
    std::vector<EdenLitScene> s;
    EdenLitScene e = b; e.name = "door-day"; s.push_back(e);
    e = b; e.name = "door-night"; set4(e.lamb[0], .17f, .17f, .17f, 1); set4(e.ldiff[0], .3f, .3f, .3f, 1); s.push_back(e);
    e = b; e.name = "firework"; e.vertexColor = 0; set4(e.tint, 0.667f, 1.0f, 1.0f, 1.0f); s.push_back(e);
    e = b; e.name = "debris (flat, far light, atlas uv)"; e.flat = 1;
    set4(e.lpos[0], -65536.0f, -21599.9f, 61874.2f, 1); e.uvScale[1] = 0.25f; e.uvOffset[1] = 0.5f; s.push_back(e);
    e = b; e.name = "golden-day"; e.lightOn[0] = 0; e.lightOn[1] = 1;
    set4(e.lpos[1], 1, 1, 0, 1); set4(e.lamb[1], .3f, .3f, .3f, 1); set4(e.ldiff[1], .3f, .3f, .3f, 1);
    set4(e.lspec[1], 1, 1, 1, 1); set4(e.matSpec, 1, 1, 1, 1); e.shininess = 100; s.push_back(e);
    e = s.back(); e.name = "golden-night"; set4(e.lamb[1], .15f, .15f, .15f, 1); set4(e.ldiff[1], .15f, .15f, .15f, 1); s.push_back(e);
    e = b; e.name = "directional + two lights"; set4(e.lpos[0], 0.3f, 0.5f, 1.0f, 0); e.lightOn[1] = 1;
    set4(e.lpos[1], -2, 1, -3, 1); set4(e.lamb[1], .05f, .05f, .05f, 1); set4(e.ldiff[1], .4f, .2f, .1f, 1);
    set4(e.lspec[1], .5f, .5f, .5f, 1); set4(e.matSpec, .8f, .8f, .8f, 1); e.shininess = 12; s.push_back(e);
    return s;
}

void drawCore(const EdenLitScene& sc, R::Tex tex) {
    static const R::VertexFormat vc = {sizeof(EdenLitVertex), {R::A_F32, 3, 0}, {R::A_U8N, 4, 24}, {R::A_F32, 2, 28},
                                       {R::A_F32, 3, 12}, {R::A_NONE, 0, 0}, {R::A_NONE, 0, 0}, {R::A_NONE, 0, 0}};
    static const R::VertexFormat vt = {sizeof(EdenLitVertex), {R::A_F32, 3, 0}, {R::A_NONE, 0, 0}, {R::A_F32, 2, 28},
                                       {R::A_F32, 3, 12}, {R::A_NONE, 0, 0}, {R::A_NONE, 0, 0}, {R::A_NONE, 0, 0}};
    R::proj().push(); R::proj().load(sc.proj);
    R::mv().push(); R::mv().load(sc.mv);
    R::uv().push(); R::uv().loadIdentity();
    R::uv().translate(sc.uvOffset[0], sc.uvOffset[1], 0.0f); R::uv().scale(sc.uvScale[0], sc.uvScale[1], 1.0f);
    const R::State st = {R::B_OFF, true, true, false, 0.0f, 0.0f};
    R::setState(st);
    R::fogOff();
    R::LightSet ls = {};
    for (int i = 0; i < 2; ++i) {
        ls.light[i].on = sc.lightOn[i] != 0;
        std::memcpy(ls.light[i].pos, sc.lpos[i], 16); std::memcpy(ls.light[i].ambient, sc.lamb[i], 16);
        std::memcpy(ls.light[i].diffuse, sc.ldiff[i], 16); std::memcpy(ls.light[i].specular, sc.lspec[i], 16);
    }
    std::memcpy(ls.modelAmbient, sc.modelAmbient, 16); std::memcpy(ls.matSpecular, sc.matSpec, 16);
    ls.shininess = sc.shininess; ls.flat = sc.flat != 0;
    R::setLights(ls);
    const R::Geometry g = {sc.vertexColor ? &vc : &vt, 0, sc.v, 0, sc.idx, R::I_U16, 0, sc.ni, R::TRIANGLES};
    R::drawLit(g, tex, R::Rgba{sc.tint[0], sc.tint[1], sc.tint[2], sc.tint[3]});
    R::uv().pop(); R::mv().pop(); R::proj().pop();
}

bool covered(const unsigned char* p) { return p[0] != 26 || p[1] != 51 || p[2] != 77; }

void writeBmp(const std::string& path, const unsigned char* rgba, int w, int h) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const int row = w * 3, pad = (4 - row % 4) % 4, img = (row + pad) * h;
    unsigned char hdr[54] = {'B', 'M'};
    auto put32 = [&](int o, int v) { for (int i = 0; i < 4; ++i) hdr[o + i] = (unsigned char)(v >> (8 * i)); };
    put32(2, 54 + img); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
    hdr[26] = 1; hdr[28] = 24; put32(34, img);
    std::fwrite(hdr, 1, 54, f);
    const unsigned char zero[3] = {0, 0, 0};
    for (int y = 0; y < h; ++y) {   // bottom-up, as GL reads
        for (int x = 0; x < w; ++x) { const unsigned char* p = rgba + ((size_t)y * w + x) * 4; const unsigned char bgr[3] = {p[2], p[1], p[0]}; std::fwrite(bgr, 1, 3, f); }
        std::fwrite(zero, 1, pad, f);
    }
    std::fclose(f);
}

}  // namespace

namespace rgl {
int litOracle(const char* outDir) {
    unsigned char texPx[4 * 4 * 4];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            unsigned char* p = &texPx[(y * 4 + x) * 4];
            const bool on = ((x ^ y) & 1) != 0;
            p[0] = on ? 250 : 200; p[1] = (unsigned char)(170 + 25 * x); p[2] = (unsigned char)(150 + 30 * y); p[3] = 255;
        }
    std::vector<EdenLitVertex> v;
    std::vector<unsigned short> idx;
    oracleMesh(v, idx);
    const std::vector<EdenLitScene> scenes = oracleScenes(v, idx, texPx);
    const int n = (int)scenes.size();
    std::vector<unsigned char> legacy((size_t)n * kOW * kOH * 4), core((size_t)n * kOW * kOH * 4);
    if (!eden_lit_oracle_legacy(scenes.data(), n, kOW, kOH, legacy.data())) {
        std::fprintf(stderr, "[lit-oracle] no legacy fixed-function context on this platform\n");
        return -1;
    }
    // The private legacy context never touched this one's state, so the caches still hold.

    GLint prevFbo = 0, vp[4];
    GLfloat cc[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    GLuint fbo = 0, rb[2] = {0, 0};
    glGenFramebuffers(1, &fbo);
    glGenRenderbuffers(2, rb);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kOW, kOH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kOW, kOH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    glViewport(0, 0, kOW, kOH);
    const R::TexDesc td = {4, 4, R::TF_RGBA8, false, false, false, false, "lit-oracle"};
    const R::Tex tex = R::texCreate(td, texPx);
    const bool wasOn = R::originalLighting();
    R::setOriginalLighting(true);
    R::beginFrame();
    for (int s = 0; s < n; ++s) {
        ::clear();
        drawCore(scenes[s], tex);
        glReadPixels(0, 0, kOW, kOH, GL_RGBA, GL_UNSIGNED_BYTE, &core[(size_t)s * kOW * kOH * 4]);
    }
    // Negative check: the debris scene shaded smooth instead of flat must NOT pass (the gate can see it).
    int negScene = -1;
    for (int s = 0; s < n; ++s) if (scenes[s].flat) negScene = s;
    std::vector<unsigned char> neg((size_t)kOW * kOH * 4);
    if (negScene >= 0) {
        EdenLitScene e = scenes[negScene];
        e.flat = 0;
        ::clear();
        drawCore(e, tex);
        glReadPixels(0, 0, kOW, kOH, GL_RGBA, GL_UNSIGNED_BYTE, neg.data());
    }
    R::endFrame();
    R::setOriginalLighting(wasOn);
    R::texDestroy(tex);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glDeleteRenderbuffers(2, rb);
    glDeleteFramebuffers(1, &fbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);

    int fails = 0;
    for (int s = 0; s < n; ++s) {
        const unsigned char* L = &legacy[(size_t)s * kOW * kOH * 4];
        const unsigned char* C = &core[(size_t)s * kOW * kOH * 4];
        int interior = 0, over = 0, maxd = 0, coverMismatch = 0, worstX = -1, worstY = -1;
        for (int y = 0; y < kOH; ++y)
            for (int x = 0; x < kOW; ++x) {
                const unsigned char* l = L + ((size_t)y * kOW + x) * 4;
                const unsigned char* c = C + ((size_t)y * kOW + x) * 4;
                if (covered(l) != covered(c)) { ++coverMismatch; continue; }
                if (!covered(l) || x == 0 || y == 0 || x == kOW - 1 || y == kOH - 1) continue;
                bool in = true;
                const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (auto& o : nb) {
                    const size_t k = ((size_t)(y + o[1]) * kOW + x + o[0]) * 4;
                    if (!covered(L + k) || !covered(C + k)) in = false;
                }
                if (!in) continue;
                ++interior;
                int d = 0;
                for (int ch = 0; ch < 3; ++ch) d = std::max(d, std::abs((int)l[ch] - (int)c[ch]));
                if (d > 2) ++over;
                if (d > maxd) { maxd = d; worstX = x; worstY = y; }
            }
        const bool ok = interior > 2000 && maxd <= 2;
        if (!ok) ++fails;
        std::printf("[lit-oracle] %s %-36s interior %5d px, max delta %3d/255, %d over 2, edge coverage differs %d px",
                    ok ? "PASS" : "FAIL", scenes[s].name, interior, maxd, over, coverMismatch);
        if (maxd > 0) {
            const unsigned char* l = L + ((size_t)worstY * kOW + worstX) * 4;
            const unsigned char* c = C + ((size_t)worstY * kOW + worstX) * 4;
            std::printf(" (worst at %d,%d: fixed %u,%u,%u lit %u,%u,%u)", worstX, worstY, l[0], l[1], l[2], c[0], c[1], c[2]);
        }
        std::printf("\n");
        if (outDir && *outDir) {
            char nm[16];
            std::snprintf(nm, sizeof nm, "%d", s);
            writeBmp(std::string(outDir) + "/oracle-" + nm + "-fixed.bmp", L, kOW, kOH);
            writeBmp(std::string(outDir) + "/oracle-" + nm + "-lit.bmp", C, kOW, kOH);
        }
    }
    if (negScene >= 0) {
        const unsigned char* L = &legacy[(size_t)negScene * kOW * kOH * 4];
        int maxd = 0;
        for (size_t i = 0; i < neg.size(); i += 4)
            if (covered(&L[i]) && covered(&neg[i]))
                for (int ch = 0; ch < 3; ++ch) maxd = std::max(maxd, std::abs((int)L[i + ch] - (int)neg[i + ch]));
        const bool ok = maxd > 2;
        if (!ok) ++fails;
        std::printf("[lit-oracle] %s negative: the flat scene shaded smooth differs by %d/255 (must exceed 2)\n",
                    ok ? "PASS" : "FAIL", maxd);
    }
    return fails;
}
}  // namespace rgl
