// render_core.cpp — the backend-independent half of the engine-owned renderer (G.5b-0; plan:
// WORKING/renderer-interface-plan-2026-10-25.md §4–§5). Owns the matrix stacks (R2: the GL shim's
// glPushMatrix/glLoadIdentity/... forward here, so ported and unported code always share one stack),
// the per-draw state, the frame/scene/2D brackets and their invariants, and the dispatch to the GL
// backend (a context is live) or the null backend (none). Plain C++, no GL.
#include "render_internal.h"
#include "render_passes.h"
#include "../../../Classes/PassTimers.h"

#include "gl/gl_context.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#if !defined(_WIN32)
#include <dlfcn.h>   // --state-trace's call-site symbols (G.5b-0)
#endif

// The pick viewport (R::pickViewport): the engine's POINT space x SCALE_*, never the drawable (see
// gl_context.h). DisplayProfile writes it through eden_gl_set_pick_viewport (gl_context_native.cpp); the
// initialiser is the 568x320@2x profile the headless build and the pre-layout frames get. Was the
// shim's kPickViewport until G.5b-7.
int g_pick_viewport[4] = {0, 0, 1136, 640};

namespace R {

// ---- matrix math: moved verbatim from the shim (its results are the pinned look) ------------------
void mat4Identity(Mat4& out) {
    std::memset(out.m, 0, sizeof(out.m));
    out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.0f;
}

void mat4Multiply(Mat4& out, const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            r.m[col * 4 + row] = sum;
        }
    }
    out = r;
}

// ES 1.1 starts EVERY stack at identity (a zero texture matrix collapsed every UI texcoord to one
// texel once; see the shim's history), so stack[0] is identity, not static zero.
MatrixStack::MatrixStack() : top_(0), version_(1), overflows_(0), underflows_(0) {
    mat4Identity(stack_[0]);
}

void MatrixStack::push() {
    if (top_ + 1 >= kMaxDepth) { ++overflows_; return; }
    stack_[top_ + 1] = stack_[top_];
    ++top_;
    ++version_;
}

void MatrixStack::pop() {
    if (top_ == 0) { ++underflows_; return; }
    --top_;
    ++version_;
}

void MatrixStack::loadIdentity() { mat4Identity(stack_[top_]); ++version_; }

void MatrixStack::load(const float* m) {
    std::memcpy(stack_[top_].m, m, sizeof(float) * 16);
    ++version_;
}

void MatrixStack::mult(const float* m) {
    Mat4 rhs;
    std::memcpy(rhs.m, m, sizeof(float) * 16);
    mat4Multiply(stack_[top_], stack_[top_], rhs);
    ++version_;
}

void MatrixStack::translate(float x, float y, float z) {
    Mat4 t;
    mat4Identity(t);
    t.m[12] = x; t.m[13] = y; t.m[14] = z;
    mat4Multiply(stack_[top_], stack_[top_], t);
    ++version_;
}

void MatrixStack::scale(float x, float y, float z) {
    Mat4 s;
    mat4Identity(s);
    s.m[0] = x; s.m[5] = y; s.m[10] = z;
    mat4Multiply(stack_[top_], stack_[top_], s);
    ++version_;
}

void MatrixStack::rotate(float angle_deg, float x, float y, float z) {
    float len = std::sqrt(x * x + y * y + z * z);
    if (len < 1e-6f) return;
    x /= len; y /= len; z /= len;
    float rad = angle_deg * 3.14159265358979323846f / 180.0f;
    float c = std::cos(rad), s = std::sin(rad), ic = 1.0f - c;
    Mat4 r;
    mat4Identity(r);
    r.m[0] = x * x * ic + c;       r.m[4] = x * y * ic - z * s;    r.m[8]  = x * z * ic + y * s;
    r.m[1] = y * x * ic + z * s;   r.m[5] = y * y * ic + c;        r.m[9]  = y * z * ic - x * s;
    r.m[2] = x * z * ic - y * s;   r.m[6] = y * z * ic + x * s;    r.m[10] = z * z * ic + c;
    mat4Multiply(stack_[top_], stack_[top_], r);
    ++version_;
}

void MatrixStack::ortho(float left, float right, float bottom, float top, float near_, float far_) {
    Mat4 o;
    mat4Identity(o);
    o.m[0]  = 2.0f / (right - left);
    o.m[5]  = 2.0f / (top - bottom);
    o.m[10] = -2.0f / (far_ - near_);
    o.m[12] = -(right + left) / (right - left);
    o.m[13] = -(top + bottom) / (top - bottom);
    o.m[14] = -(far_ + near_) / (far_ - near_);
    mat4Multiply(stack_[top_], stack_[top_], o);
    ++version_;
}

void MatrixStack::frustum(float left, float right, float bottom, float top, float near_, float far_) {
    Mat4 f;
    std::memset(f.m, 0, sizeof(f.m));
    f.m[0]  = (2.0f * near_) / (right - left);
    f.m[5]  = (2.0f * near_) / (top - bottom);
    f.m[8]  = (right + left) / (right - left);
    f.m[9]  = (top + bottom) / (top - bottom);
    f.m[10] = -(far_ + near_) / (far_ - near_);
    f.m[11] = -1.0f;
    f.m[14] = -(2.0f * far_ * near_) / (far_ - near_);
    mat4Multiply(stack_[top_], stack_[top_], f);
    ++version_;
}

MatrixStack& mv()   { static MatrixStack s; return s; }
MatrixStack& proj() { static MatrixStack s; return s; }
MatrixStack& uv()   { static MatrixStack s; return s; }

void uvScaleOffset(float out[4]) {
    const Mat4& t = uv().top();
    out[0] = t.m[0]; out[1] = t.m[5]; out[2] = t.m[12]; out[3] = t.m[13];
}

}  // namespace R

namespace rcore {

DrawState& ds() { static DrawState s; return s; }
Invariants& inv() { static Invariants i; return i; }

namespace {
bool g_inFrame = false;
int  g_frameDepth[3] = {0, 0, 0};
int  g_open2D = 0, g_openScene = 0;
bool g_contracts = false;
bool g_originalLighting = true;   // Settings › Video › Original lighting (plan §7), default on

int frame_mark_depth(int i) {
    return i == 0 ? R::mv().depth() : i == 1 ? R::proj().depth() : R::uv().depth();
}
}  // namespace

void fail(const char* what) {
    if (inv().firstFailure.empty()) inv().firstFailure = what;
}
bool inFrame() { return g_inFrame; }
void setPassContracts(bool on) { g_contracts = on; }
bool passContracts() { return g_contracts; }

void noteDraw(bool renderer) {
    ++inv().draws;
    if (renderer) ++inv().rdraws;
    if (!g_inFrame) {
        if (inv().drawsOutsideFrame++ == 0) fail("a draw outside beginFrame/endFrame");
    }
}

}  // namespace rcore

using namespace rcore;

// G.5b-0: the scene bracket is eden_scene_pass_begin/end's (gl_context_native.cpp) until G.5b-6 moves
// World onto R::beginScene; both ends report here so the invariant holds either way.
extern "C" void eden_render_note_scene(int begin) {
    if (begin) { ++g_openScene; return; }
    if (--g_openScene < 0) { g_openScene = 0; if (inv().unbalancedScene++ == 0) fail("endScene without beginScene"); }
}

// The signs pass contract (plan §5) was the ES 1.1 state the signs pass entered and left with (texture
// on, lighting off, blend off, cull on, array buffer 0), read from the shim. With the shim gone (G.5b-7)
// no draw inherits state, so the check is only counted: the pass still brackets correctly.
extern "C" void eden_render_pass_contract(int pass, int begin) {
    (void)begin;
    if (!g_contracts || pass != P_R_SIGNS) return;
    ++inv().contractChecks;
}

// ---- --state-trace (G.5b-0; the shim's half moved here in G.5b-7) ------------------------------------
// One line per draw while a harness has the trace open (one frame of --scene-shot). The call site is the
// R:: entry's return address, symbolised where dladdr can (exported symbols on macOS; the address elsewhere).
static FILE* g_trace = nullptr;
static long  g_trace_n = 0;
extern "C" void eden_gl_trace_begin(FILE* f) { g_trace = f; g_trace_n = 0; }
extern "C" void eden_gl_trace_end(void) { g_trace = nullptr; }
static void traceLine(void* site, const char* body) {
    char sym[160];
    std::snprintf(sym, sizeof(sym), "%p", site);
#if !defined(_WIN32)
    Dl_info info;
    if (dladdr(site, &info) && info.dli_sname)
        std::snprintf(sym, sizeof(sym), "%s+%ld", info.dli_sname,
                      (long)((const char*)site - (const char*)info.dli_saddr));
#endif
    std::fprintf(g_trace, "draw=%ld pass=%s cat=%s site=%s call=R %s stack=%d/%d/%d\n", g_trace_n++,
                 eden_render_pass_name(eden_render_pass_innermost()),
                 eden_render_category_name(eden_render_draw_category()), sym, body,
                 R::mv().depth(), R::proj().depth(), R::uv().depth());
}

namespace R {

void beginFrame() {
    if (g_inFrame) { if (inv().nestedFrames++ == 0) fail("beginFrame inside a frame"); }
    g_inFrame = true;
    for (int i = 0; i < 3; ++i) g_frameDepth[i] = frame_mark_depth(i);
    g_open2D = 0;
    if (eden_gl_have_context()) rgl::beginFrame();
}

void endFrame() {
    if (!g_inFrame) { if (inv().endWithoutBegin++ == 0) fail("endFrame without beginFrame"); return; }
    ++inv().frames;
    bool stacks = true;
    for (int i = 0; i < 3; ++i) if (frame_mark_depth(i) != g_frameDepth[i]) stacks = false;
    if (!stacks) {
        char msg[160];
        std::snprintf(msg, sizeof msg, "matrix stacks unbalanced over a frame: mv %d->%d proj %d->%d uv %d->%d",
                      g_frameDepth[0], frame_mark_depth(0), g_frameDepth[1], frame_mark_depth(1),
                      g_frameDepth[2], frame_mark_depth(2));
        if (inv().stackUnbalancedFrames++ == 0) fail(msg);
    }
    if (g_open2D != 0) { if (inv().unbalanced2D++ == 0) fail("begin2D without end2D in a frame"); g_open2D = 0; }
    if (g_openScene != 0) { if (inv().unbalancedScene++ == 0) fail("beginScene without endScene in a frame"); g_openScene = 0; }
    if (g_pass.track && (eden_render_pass_depth() != 0 || eden_render_pass_unbalanced() != 0)) {
        if (inv().passUnbalancedFrames++ == 0) fail("pass_begin/pass_end unbalanced over a frame");
        eden_render_pass_reset();
    }
    g_inFrame = false;
    if (eden_gl_have_context()) rgl::endFrame();
}

extern "C" void eden_scene_pass_begin(void);
extern "C" void eden_scene_pass_end(void);
void beginScene() { eden_scene_pass_begin(); }
void endScene()   { eden_scene_pass_end(); }

void begin2D(float w, float h, float zfar) {
    ++g_open2D;
    proj().push(); proj().loadIdentity(); proj().ortho(0, w, 0, h, -zfar, zfar);
    mv().push();   mv().loadIdentity();
}

void end2D() {
    if (g_open2D <= 0) { if (inv().unbalanced2D++ == 0) fail("end2D without begin2D"); return; }
    --g_open2D;
    mv().pop();
    proj().pop();
}

// The clear (G.5b-7; was the shim's glClear/glClearColor). With a --scene-shot pass mask the sky may be
// masked out and prepareScene clears only depth (the sky covers the frame), so a masked frame clears
// colour too: pass-alone captures then start from the clear colour, not from the last frame.
static Rgba g_clearColor = { 0, 0, 0, 0 };
void setClearColor(const Rgba& c) { g_clearColor = c; }
void clear(bool color, bool depth) {
    if (eden_render_mask_active() && depth) color = true;
    if (!color && !depth) return;
    if (eden_gl_have_context()) rgl::clear(color, depth, g_clearColor);
}

void setState(const State& s) { ds().state = s; }
void setFog(const Rgba& c, float start, float end) {
    ds().fogOn = true; ds().fogColor = c; ds().fogStart = start; ds().fogEnd = end;
}
void fogOff() { ds().fogOn = false; }
void setLights(const LightSet& l) { ds().lights = l; }

bool originalLighting() { return g_originalLighting; }
void setOriginalLighting(bool on) { g_originalLighting = on; }

Tex texCreate(const TexDesc& d, const void* px) {
    return eden_gl_have_context() ? rgl::texCreate(d, px) : rnull::texCreate(d);
}
void texUpdate(Tex t, int x, int y, int w, int h, TexFormat f, const void* px) {
    if (eden_gl_have_context()) rgl::texUpdate(t, x, y, w, h, f, px);
}
void texDestroy(Tex t) {
    if (eden_gl_have_context()) rgl::texDestroy(t); else rnull::texDestroy(t);
}
void texSampling(Tex t, const TexDesc& d) {
    if (eden_gl_have_context()) rgl::texSampling(t, d);
}
Buf bufCreate(BufKind k, Usage u) {
    return eden_gl_have_context() ? rgl::bufCreate(k, u) : rnull::bufCreate();
}
void bufData(Buf b, const void* data, size_t bytes) {
    if (eden_gl_have_context()) rgl::bufData(b, data, bytes);
}
void bufSubData(Buf b, size_t offset, const void* data, size_t bytes) {
    if (eden_gl_have_context()) rgl::bufSubData(b, offset, data, bytes);
}
void bufDestroy(Buf b) {
    if (eden_gl_have_context()) rgl::bufDestroy(b); else rnull::bufDestroy(b);
}

// One --state-trace line per renderer draw: the fields of the shim's line a ported call site must state.
static void traceDraw(void* site, const char* prog, const Geometry& g, Tex t, const Rgba& tint, float alphaRef) {
    if (!g_trace) return;
    static const char* const kPrim[] = {"tri", "strip", "fan", "lines", "lstrip", "points"};
    static const char* const kBlend[] = {"off", "SA,1-SA", "SA,1", "1,1"};
    const DrawState& d = ds();
    char col[64], fog[96], off[48] = "", alpha[24] = "off", lit[160] = "";
    if (g.fmt && g.fmt->color.type != A_NONE) std::snprintf(col, sizeof col, "array");
    else std::snprintf(col, sizeof col, "tint(%.3f,%.3f,%.3f,%.3f)", tint.r, tint.g, tint.b, tint.a);
    if (d.fogOn) std::snprintf(fog, sizeof fog, "%.2f..%.2f(%.3f,%.3f,%.3f)", d.fogStart, d.fogEnd, d.fogColor.r, d.fogColor.g, d.fogColor.b);
    else std::snprintf(fog, sizeof fog, "off");
    if (d.state.offsetFactor != 0.0f || d.state.offsetUnits != 0.0f)
        std::snprintf(off, sizeof off, " offset=%g,%g", d.state.offsetFactor, d.state.offsetUnits);
    if (alphaRef >= 0.0f) std::snprintf(alpha, sizeof alpha, ">%.3f", alphaRef);
    if (!std::strcmp(prog, "lit")) {
        const LightSet& L = d.lights;
        std::snprintf(lit, sizeof lit, " lights=%d%d%s l0=(%.2f,%.2f,%.2f,%.0f a%.2f d%.2f) amb%.2f spec%.2f^%g",
                      L.light[0].on, L.light[1].on, L.flat ? " flat" : "", L.light[0].pos[0], L.light[0].pos[1],
                      L.light[0].pos[2], L.light[0].pos[3], L.light[0].ambient[0], L.light[0].diffuse[0],
                      L.modelAmbient[0], L.matSpecular[0], L.shininess);
        if (!g_originalLighting) std::snprintf(lit, sizeof lit, " (Original lighting off: world)");
    }
    float so[4];
    uvScaleOffset(so);
    const bool uvI = so[0] == 1.0f && so[1] == 1.0f && so[2] == 0.0f && so[3] == 0.0f;
    char uv[64] = "I";
    if (!uvI) std::snprintf(uv, sizeof uv, "X(s%g,%g t%g,%g)", so[0], so[1], so[2], so[3]);
    char body[640];
    std::snprintf(body, sizeof body, "prim=%s n=%d prog=%s tex=%u col=%s fog=%s blend=%s depth=%d/%d cull=%d%s alpha=%s%s texmat=%s",
                  kPrim[g.prim], g.count, prog, (unsigned)t, col, fog, kBlend[d.state.blend], d.state.depthTest ? 1 : 0,
                  d.state.depthWrite ? 1 : 0, d.state.cull ? 1 : 0, off, alpha, lit, uv);
    traceLine(site, body);
}

void drawWorld(const Geometry& g, Tex t, const Rgba& tint, float alphaRef) {
    if (!eden_render_draw_allowed()) return;
    traceDraw(__builtin_return_address(0), "world", g, t, tint, alphaRef);
    noteDraw(true);
    if (eden_gl_have_context()) rgl::drawWorld(g, t, tint, alphaRef); else rnull::draw(g);
}
void draw2D(const Geometry& g, Tex t, const Rgba& tint) {
    if (!eden_render_draw_allowed()) return;
    traceDraw(__builtin_return_address(0), "ui", g, t, tint, -1.0f);
    noteDraw(true);
    if (eden_gl_have_context()) rgl::draw2D(g, t, tint); else rnull::draw(g);
}
void drawSprites(const Geometry& g, Tex t, const SpriteParams& p) {
    if (!eden_render_draw_allowed()) return;
    traceDraw(__builtin_return_address(0), "sprite", g, t, Rgba{1, 1, 1, 1}, -1.0f);
    noteDraw(true);
    if (eden_gl_have_context()) rgl::drawSprites(g, t, p); else rnull::draw(g);
}
void drawSkinned(const Geometry& g, Tex t, const Rgba& tint, const float* palette, int nmat, int bonesPerVertex) {
    if (!eden_render_draw_allowed()) return;
    traceDraw(__builtin_return_address(0), "skinned", g, t, tint, -1.0f);
    noteDraw(true);
    if (eden_gl_have_context()) rgl::drawSkinned(g, t, tint, palette, nmat, bonesPerVertex); else rnull::draw(g);
}
void drawLit(const Geometry& g, Tex t, const Rgba& tint) {
    if (!eden_render_draw_allowed()) return;
    traceDraw(__builtin_return_address(0), "lit", g, t, tint, -1.0f);
    noteDraw(true);
    if (!eden_gl_have_context()) { rnull::draw(g); return; }
    if (g_originalLighting) rgl::drawLit(g, t, tint);
    else rgl::drawWorld(g, t, tint, -1.0f);   // R8: OFF is today's unlit draw
}

void pickViewport(int out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = g_pick_viewport[i];
}

bool readPixels(int x, int y, int w, int h, void* rgba) {
    if (eden_gl_have_context()) return rgl::readPixels(x, y, w, h, rgba);
    if (rgba && w > 0 && h > 0) std::memset(rgba, 0, (size_t)w * (size_t)h * 4u);
    return false;
}

void drawableSize(int& w, int& h) { eden_gl_context_get_drawable_size(&w, &h); }

unsigned takeError() { return eden_gl_have_context() ? rgl::takeError() : 0u; }
const char* driverString(DriverString w) { return eden_gl_have_context() ? rgl::driverString((int)w) : ""; }

}  // namespace R

// For the harness (--render-selftest).
extern "C" void eden_render_set_contracts(int on) { setPassContracts(on != 0); }
extern "C" const rcore::Invariants* eden_render_invariants(void) { return &inv(); }
extern "C" void eden_render_reset_invariants(void) { inv() = Invariants(); }
