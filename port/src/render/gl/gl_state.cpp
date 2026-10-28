// gl_state.cpp — see gl_state.h. Most of this moved verbatim out of the ES 1.1 shim (G.5b-0); its
// LOGICAL half went with the shim (G.5b-7).
#include "gl_state.h"
#include "gl_context.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace rgl {

// ---- accounting ----------------------------------------------------------------------------------
// Not debug decoration: the dirty tracking exists to cut calls, and the port's rule is "measure,
// never extrapolate". `issued` counts the setup calls actually made this frame; `elided` the ones the
// caches skipped. eden_gl_stat() reports the last completed frame.
static DrawStats g_stats, g_statsLast;
DrawStats& stats() { return g_stats; }
const DrawStats& statsLast() { return g_statsLast; }

void bindAttribLocations(GLuint prog) {
    glBindAttribLocation(prog, ATTR_POSITION,    "a_position");
    glBindAttribLocation(prog, ATTR_COLOR,       "a_color");
    glBindAttribLocation(prog, ATTR_TEXCOORD,    "a_texcoord");
    glBindAttribLocation(prog, ATTR_NORMAL,      "a_normal");
    glBindAttribLocation(prog, ATTR_POINTSIZE,   "a_pointsize");
    glBindAttribLocation(prog, ATTR_MATRIXINDEX, "a_matrixindex");
    glBindAttribLocation(prog, ATTR_WEIGHT,      "a_weight");
}

AttrCache g_attrCache[ATTR_COUNT];

GLuint g_bound_array_buffer = 0;
GLuint g_bound_element_buffer = 0;
GLuint g_path_array_buffer = 0;
bool   g_path_ebo_dirty = false;

static GLuint g_bound_program = 0;    // 0 = unknown/none
static GLenum g_active_texture = 0;   // 0 = unknown; GL's own default is GL_TEXTURE0

void useProgram(GLuint prog) {
    if (g_bound_program == prog) { stat_elided(); return; }
    glUseProgram(prog);
    g_bound_program = prog;
    stat_issued();
}

void activeTexture0() {
    if (g_active_texture == GL_TEXTURE0) { stat_elided(); return; }
    glActiveTexture(GL_TEXTURE0);
    g_active_texture = GL_TEXTURE0;
    stat_issued();
}

void bindPathArray(GLuint buf) {
    if (g_path_array_buffer == buf) { stat_elided(); return; }
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    g_path_array_buffer = buf;
    stat_issued();
}

void bindPathElements(GLuint buf) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf);
    stat_issued();
    // Only a binding that differs from the engine's needs putting back.
    if (buf != g_bound_element_buffer) g_path_ebo_dirty = true;
}

void restoreBindings() {
    if (g_path_array_buffer != g_bound_array_buffer) {
        glBindBuffer(GL_ARRAY_BUFFER, g_bound_array_buffer);
        g_path_array_buffer = g_bound_array_buffer;
        stat_issued();
    } else stat_elided();
    if (g_path_ebo_dirty) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_bound_element_buffer);
        g_path_ebo_dirty = false;
        stat_issued();
    } else stat_elided();
}

void attribDisable(int slot) {
    AttrCache& c = g_attrCache[slot];
    if (c.enabled) { glDisableVertexAttribArray(slot); c.enabled = false; stat_issued(); }
    else stat_elided();
}

void attribPointer(int slot, GLuint buf, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* ptr) {
    AttrCache& c = g_attrCache[slot];
    if (!c.enabled) { glEnableVertexAttribArray(slot); c.enabled = true; stat_issued(); }
    else stat_elided();
    if (c.buffer == buf && c.size == size && c.type == type && c.normalized == norm &&
        c.stride == stride && c.pointer == ptr) { stat_elided(); return; }
    bindPathArray(buf);
    glVertexAttribPointer(slot, size, type, norm, stride, ptr);
    stat_issued();
    c.buffer = buf; c.size = size; c.type = type; c.normalized = norm; c.stride = stride; c.pointer = ptr;
}

// Generic attribute values are context state, not program state: cached, never invalidated by a
// program change. NaN-initialised so the first set always issues.
static float g_constAttr[ATTR_COUNT][4];
static bool  g_constKnown[ATTR_COUNT];
void attribConstant(int slot, float x, float y, float z, float w) {
    float* v = g_constAttr[slot];
    if (g_constKnown[slot] && v[0] == x && v[1] == y && v[2] == z && v[3] == w) { stat_elided(); return; }
    glVertexAttrib4f(slot, x, y, z, w);
    v[0] = x; v[1] = y; v[2] = z; v[3] = w;
    g_constKnown[slot] = true;
    stat_issued();
}

// ---- M0 buffer-byte accounting (moved from the shim) -------------------------------------------------
// How many bytes of vertex/index data are resident in driver-side buffers, and the churn. `bytes` is
// what the app asked glBufferData for; the driver rounds each name up to at least a page.
static std::unordered_map<GLuint, size_t> g_glbuf_sizes;
static size_t   g_glbuf_bytes      = 0;
static size_t   g_glbuf_peakBytes  = 0;
static unsigned g_glbuf_count      = 0;
static unsigned g_glbuf_peakCount  = 0;
static unsigned g_glbuf_creates    = 0;
static unsigned g_glbuf_deletes    = 0;

void noteBufferCreates(unsigned n) { g_glbuf_creates += n; }

void noteBufferDataFor(GLuint name, size_t size) {
    if (name == 0) return;   // client-memory path / no buffer bound
    auto it = g_glbuf_sizes.find(name);
    if (it == g_glbuf_sizes.end()) {
        g_glbuf_sizes.emplace(name, size);
        g_glbuf_bytes += size;
        ++g_glbuf_count;
    } else {
        g_glbuf_bytes = g_glbuf_bytes - it->second + size;
        it->second = size;
    }
    if (g_glbuf_bytes > g_glbuf_peakBytes) g_glbuf_peakBytes = g_glbuf_bytes;
    if (g_glbuf_count > g_glbuf_peakCount) g_glbuf_peakCount = g_glbuf_count;
}

static void noteBufferDelete(GLuint name) {
    if (name == 0) return;
    ++g_glbuf_deletes;
    auto it = g_glbuf_sizes.find(name);
    if (it == g_glbuf_sizes.end()) return;
    g_glbuf_bytes -= it->second;
    --g_glbuf_count;
    g_glbuf_sizes.erase(it);
}

// GL detaches a deleted buffer from EVERY binding point, the ARRAY_BUFFER binding and every attribute
// specified against it, which revert to 0. The attribute cache would otherwise claim "slot 0 points at
// buffer 7" after 7 was deleted (and recycled by glGenBuffers), skip the glVertexAttribPointer, and draw
// from a detached attribute. TerrainChunk deletes and regenerates its VBOs as chunks stream.
void forgetDeletedBuffers(GLsizei n, const GLuint* buffers) {
    if (!buffers) return;
    for (GLsizei i = 0; i < n; ++i) {
        const GLuint name = buffers[i];
        if (name == 0) continue;
        noteBufferDelete(name);
        for (int slot = 0; slot < ATTR_COUNT; ++slot)
            if (g_attrCache[slot].buffer == name) g_attrCache[slot] = AttrCache();
        if (g_bound_array_buffer == name)   g_bound_array_buffer = 0;
        if (g_bound_element_buffer == name) g_bound_element_buffer = 0;
        if (g_path_array_buffer == name)    g_path_array_buffer = 0;
    }
}

// ---- ACTUAL -------------------------------------------------------------------------------------------
// What GL holds. -1 / ~0 sentinels mean "unknown": the first apply after a (new) context issues all.
namespace {
struct Actual {
    GLuint  tex2d;
    int     blend, depthTest, depthMask, cull, polyOffset;
    GLenum  bsrc, bdst;
    GLfloat offFactor, offUnits;
    bool    offKnown, funcKnown;
};
Actual g_actual;
void actual_unknown() {
    g_actual.tex2d = 0xFFFFFFFFu;
    g_actual.blend = g_actual.depthTest = g_actual.depthMask = g_actual.cull = g_actual.polyOffset = -1;
    g_actual.bsrc = g_actual.bdst = 0xFFFFFFFFu;
    g_actual.offFactor = g_actual.offUnits = 0.0f;
    g_actual.offKnown = g_actual.funcKnown = false;
}
struct ActualInit { ActualInit() { actual_unknown(); } } g_actualInit;

inline void cap(GLenum c, bool want, int& have) {
    if (have == (want ? 1 : 0)) { stat_elided(); return; }
    if (want) glEnable(c); else glDisable(c);
    have = want ? 1 : 0;
    stat_issued();
}
inline void bindTex(GLuint t) {
    if (g_actual.tex2d == t) { stat_elided(); return; }
    activeTexture0();
    glBindTexture(GL_TEXTURE_2D, t);
    g_actual.tex2d = t;
    stat_issued();
}
inline void blendFunc(GLenum s, GLenum d) {
    if (g_actual.funcKnown && g_actual.bsrc == s && g_actual.bdst == d) { stat_elided(); return; }
    glBlendFunc(s, d);
    g_actual.bsrc = s; g_actual.bdst = d; g_actual.funcKnown = true;
    stat_issued();
}
inline void depthMask(bool on) {
    if (g_actual.depthMask == (on ? 1 : 0)) { stat_elided(); return; }
    glDepthMask(on ? GL_TRUE : GL_FALSE);
    g_actual.depthMask = on ? 1 : 0;
    stat_issued();
}
inline void polygonOffset(GLfloat f, GLfloat u) {
    if (g_actual.offKnown && g_actual.offFactor == f && g_actual.offUnits == u) { stat_elided(); return; }
    glPolygonOffset(f, u);
    g_actual.offFactor = f; g_actual.offUnits = u; g_actual.offKnown = true;
    stat_issued();
}
}  // namespace

void bindTextureNow(GLuint t) { bindTex(t); }
void depthWriteNow(bool on) { depthMask(on); }

void applyRenderer(const R::State& s, GLuint tex) {
    bindTex(tex);
    cap(GL_BLEND, s.blend != R::B_OFF, g_actual.blend);
    if (s.blend == R::B_ALPHA) blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    else if (s.blend == R::B_ADD) blendFunc(GL_SRC_ALPHA, GL_ONE);
    else if (s.blend == R::B_ONE) blendFunc(GL_ONE, GL_ONE);
    cap(GL_DEPTH_TEST, s.depthTest, g_actual.depthTest);
    depthMask(s.depthWrite);
    cap(GL_CULL_FACE, s.cull, g_actual.cull);
    const bool off = s.offsetFactor != 0.0f || s.offsetUnits != 0.0f;
    cap(GL_POLYGON_OFFSET_FILL, off, g_actual.polyOffset);
    if (off) polygonOffset(s.offsetFactor, s.offsetUnits);
}

void forgetTexture(GLuint name) {
    if (name == 0) return;
    if (g_actual.tex2d == name) g_actual.tex2d = 0;
}

void invalidate() {
    g_bound_program = 0;
    g_active_texture = 0;
    for (int i = 0; i < ATTR_COUNT; ++i) { g_attrCache[i] = AttrCache(); g_constKnown[i] = false; }
    g_path_array_buffer = 0;
    g_path_ebo_dirty = false;
    actual_unknown();
}

}  // namespace rgl

using namespace rgl;

// ---- the accounting's exports (unchanged names and meanings; moved from the shim) ------------------
extern "C" {

// The frame boundary for the accounting. Called from the host's drawFrame, not inferred from glClear:
// prepareScene does not clear colour, so a colour-bit boundary froze the counters in play.
void eden_gl_stats_frame_boundary(void) {
    g_statsLast = g_stats;
    g_stats = DrawStats();
}

// which: 0=draw calls, 1=setup GL calls issued, 2=setup GL calls elided by the caches,
// 3=issued+elided, 4=vertices. Per LAST completed frame.
int eden_gl_stat(int which) {
    switch (which) {
        case 0: return (int)g_statsLast.draws;
        case 1: return (int)g_statsLast.issued;
        case 2: return (int)g_statsLast.elided;
        case 3: return (int)(g_statsLast.issued + g_statsLast.elided);
        case 4: return (int)g_statsLast.verts;
        default: return 0;
    }
}

// P.1b: the same counters for the frame still being drawn (Classes/PassTimers.h differences them).
long eden_gl_stat_live(int which) {
    switch (which) {
        case 0: return g_stats.draws;
        case 1: return g_stats.issued;
        case 2: return g_stats.elided;
        case 3: return g_stats.issued + g_stats.elided;
        case 4: return g_stats.verts;
        default: return 0;
    }
}

// P.1b's opt-in glFinish-per-pass mode. Never called unless a harness asked for it.
void eden_gl_finish(void) {
    if (eden_gl_have_context()) glFinish();
}

// M0: resident GL buffer bytes + create/delete churn, JSON. Not diagnostics-gated: read in release.
const char* eden_debug_gl_buffer_bytes(void) {
    static char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"count\":%u,\"bytes\":%llu,\"peakCount\":%u,\"peakBytes\":%llu,"
        "\"creates\":%u,\"deletes\":%u}",
        g_glbuf_count, (unsigned long long)g_glbuf_bytes,
        g_glbuf_peakCount, (unsigned long long)g_glbuf_peakBytes,
        g_glbuf_creates, g_glbuf_deletes);
    return buf;
}

// Reset the peak/churn accumulators (not the live count/bytes) so a probe can measure one window.
void eden_debug_gl_buffer_bytes_reset(void) {
    g_glbuf_peakBytes = g_glbuf_bytes;
    g_glbuf_peakCount = g_glbuf_count;
    g_glbuf_creates = 0;
    g_glbuf_deletes = 0;
}

}  // extern "C"
