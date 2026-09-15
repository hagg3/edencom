// gl_fixed_function.cpp — the ES 1.1 fixed-function -> WebGL2/desktop-GL translator: matrix
// stack, client arrays, fog/lighting/texture-env state capture, and the draw path (two shader
// programs). Split out of gl_es1_shim.cpp in Phase N Stage 0.2
// (WORKING/native-migration-plan-2026-09-04.md) — this file has zero Emscripten dependency and
// is cmake-shared with native; only gl_context_web.cpp (context create/destroy/resize, the two
// webglcontext{lost,restored} callbacks) is Emscripten-specific, and native gets its own
// gl_context_native.cpp against the same interface (gl_es1_shim.h) and the same shared state
// (gl_shim_internal.h).
//
// Below is the original (partial D2 implementation) header, otherwise unchanged; the required
// deliverable is gl_es1_shim.h's interface/inventory, this is a down payment on Stage P2.
//
// Implements: the full matrix stack (GROUP 1) tracked in software + glGetFloatv's
// GL_MODELVIEW_MATRIX/GL_PROJECTION_MATRIX/GL_TEXTURE_MATRIX readback (needed by Camera.mm's
// picking raycast — see gl_es1_shim.h GROUP 6 note), plus glColor4f/glColor4ub as a
// software-tracked "current color" (mimicking ES1's vertex-attribute-less immediate color).
//
// NOT implemented here (left TODO for Stage P2, "GL surface + fixed-function shim + first
// frame" in web-port-plan.md): the actual WebGL2 draw path — translating
// glVertexPointer/glColorPointer/glTexCoordPointer + glDrawArrays/glDrawElements into bound
// VBOs + a real shader program with the baked-vertex-color + texture-matrix-UV-transform
// uniform (docs/rendering.md vertex formats). That needs a live WebGL2 context (created by
// src/seam/EAGLView_web.* in P2) to test against, which does not exist yet.
//
// Math note: matrices are column-major 4x4 float[16], matching GL's glLoadMatrixf/
// glMultMatrixf convention (so callers that build matrices by hand — Classes/project.c's
// GLU port — need no transposition).

// EDEN_GL_NO_GUARD: this translation unit — and only this one — sees the REAL passthrough
// names (glEnable, glGenBuffers, ...) rather than the eden_gl_* guard macros, because it is
// what forwards to them once a context exists. See gl_es1_shim.h GROUP 2b.
#define EDEN_GL_NO_GUARD 1
#include "gl_es1_shim.h"
#include "gl_shim_internal.h"
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <unordered_map>
// GROUP 2d's draw path is written against the GLES2 entry points (shaders, uniforms,
// vertex attributes) that the ES1 header above does not declare. Including both is safe:
// every shared enum and typedef has identical definitions in the two headers, and the
// function sets are disjoint apart from prototypes that match exactly.
#include <GLES2/gl2.h>

namespace eden_gl_shim {

struct Mat4 {
    GLfloat m[16];
};

static void mat4_identity(Mat4& out) {
    std::memset(out.m, 0, sizeof(out.m));
    out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.0f;
}

// out = a * b (GL column-major composition: applying `out` to a vector applies b first, then a)
static void mat4_multiply(Mat4& out, const Mat4& a, const Mat4& b) {
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

// Fixed small stack depth — the engine never nests deeply (per-chunk translate is 1-2 levels,
// menu/hud ortho passes are 1 level). TODO P2: bump if a real device trace shows deeper use.
constexpr int kMaxStackDepth = 16;

struct MatrixMode {
    Mat4 stack[kMaxStackDepth];
    int top = 0; // stack[top] is current
    // ES1 defaults EVERY matrix stack's current matrix to IDENTITY, not zero. Static storage
    // would otherwise zero-init stack[0] to the all-zeros matrix. That is masked for
    // PROJECTION/MODELVIEW (Graphics::prepareMenu always glLoadIdentity's them first), but NOT
    // for the TEXTURE matrix: the menu path never calls glMatrixMode(GL_TEXTURE) (all 9 such
    // sites are play-mode Terrain/Fire/BlockBreak), yet the draw shader applies u_texmat to
    // every textured draw. A zero texture matrix collapses every UI texcoord to (0,0) — one
    // corner texel per quad — which renders the whole menu black with no GL error. Seeding the
    // current matrix to identity here reproduces the real driver's initial state. (Only stack[0]
    // matters: glPushMatrix duplicates the current matrix upward.)
    MatrixMode() { mat4_identity(stack[0]); }
};

static MatrixMode g_modelview;
static MatrixMode g_projection;
static MatrixMode g_texture;
static GLenum g_matrix_mode = GL_MODELVIEW;

static MatrixMode& active() {
    switch (g_matrix_mode) {
        case GL_PROJECTION: return g_projection;
        case GL_TEXTURE:    return g_texture;
        case GL_MODELVIEW:
        default:            return g_modelview;
    }
}

// Software-tracked "current color" — ES1 immediate-mode state that GLES2/WebGL2 has no
// equivalent for; TODO P2: fold into the shim's vertex shader as a uniform used when
// GL_COLOR_ARRAY is disabled (matches ES1 semantics: per-vertex color array wins if enabled,
// else the last glColor4f/glColor4ub value applies to all vertices).
static GLfloat g_current_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

// Client-array state (GROUP 2). ES1 semantics that matter for the translation below:
//   * the {size,type,stride,pointer} of an array is latched when gl*Pointer is called, and so
//     is the GL_ARRAY_BUFFER binding AT THAT MOMENT — a later glBindBuffer does not retarget an
//     already-specified array. The engine relies on this: TerrainChunk.mm binds its VBO, then
//     specifies three arrays as byte offsets into it, then draws.
//   * stride 0 means "tightly packed", i.e. an effective stride of size*sizeof(type).
struct ClientArray {
    bool enabled = false;
    GLint size = 0;
    GLenum type = 0;
    GLsizei stride = 0;
    const void* pointer = nullptr;
    GLuint buffer = 0;      // GL_ARRAY_BUFFER binding latched at gl*Pointer time; 0 = client memory
};

enum AttrSlot { ATTR_POSITION = 0, ATTR_COLOR, ATTR_TEXCOORD, ATTR_NORMAL, ATTR_POINTSIZE,
                ATTR_MATRIXINDEX, ATTR_WEIGHT, ATTR_COUNT };
static ClientArray g_arrays[ATTR_COUNT];

// GROUP 8 — GL_OES_matrix_palette state. ES1's palette is a set of independent current matrices
// (no stack, no push/pop): glMatrixMode(GL_MATRIX_PALETTE_OES) + glCurrentPaletteMatrixOES(j)
// selects one, and glLoadMatrixf writes it. Classes/Model.mm's DrawModel does exactly that and
// nothing else, so only "select + load" is modelled here.
//
// 16 is a deliberate ceiling, not a guess about the data: ES1's own initial
// GL_MAX_PALETTE_MATRICES_OES is 9, and the POD exporter bone-batches each mesh so no batch ever
// needs more than that. glCurrentPaletteMatrixOES warns once if the data disagrees rather than
// writing out of bounds.
constexpr int kMaxPaletteMatrices = 16;
static Mat4 g_palette[kMaxPaletteMatrices];
static int  g_current_palette = 0;
static bool g_matrix_palette_enabled = false;

// ES1-only enums. Emscripten's GLES1 headers are not guaranteed to declare them (the shim is
// compiled against GLES2/gl2.h as well), so define what is missing rather than depend on it.
#ifndef GL_MATRIX_PALETTE_OES
#define GL_MATRIX_PALETTE_OES        0x8840
#endif
#ifndef GL_MAX_PALETTE_MATRICES_OES
#define GL_MAX_PALETTE_MATRICES_OES  0x8842
#endif
#ifndef GL_MATRIX_INDEX_ARRAY_OES
#define GL_MATRIX_INDEX_ARRAY_OES    0x8844
#endif
#ifndef GL_WEIGHT_ARRAY_OES
#define GL_WEIGHT_ARRAY_OES          0x86AD
#endif

// Same lesson as MatrixMode's constructor (pass 14): static storage would leave these as the
// all-zeros matrix, which collapses every skinned vertex to the origin with no GL error. A batch
// only loads the bones it actually uses, so unused slots must already be identity.
struct PaletteInit {
    PaletteInit() { for (int i = 0; i < kMaxPaletteMatrices; ++i) mat4_identity(g_palette[i]); }
};
static PaletteInit g_palette_init;

// ES1-only enable caps (GROUP 2b's glEnable/glDisable guard filters these OUT before they reach
// WebGL, which would answer GL_INVALID_ENUM for every one of them). Tracked here because the
// draw path needs them as shader uniforms.
static bool g_texture2d_enabled = false;
static bool g_fog_enabled = false;
static bool g_alpha_test_enabled = false;
static bool g_lighting_enabled = false;
static bool g_point_sprite_enabled = false;

// GROUP 3 fog state (linear only — Graphics::setZFAR is the only writer, docs/rendering.md).
static GLenum  g_fog_mode = GL_LINEAR;
static GLfloat g_fog_start = 0.0f;
static GLfloat g_fog_end = 1.0f;
static GLfloat g_fog_density = 1.0f;
static GLfloat g_fog_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};

// GROUP 6 alpha test (emulated with `discard`, the only way in GLES2+).
static GLenum   g_alpha_func = GL_ALWAYS;
static GLclampf g_alpha_ref = 0.0f;

// GROUP 5 texture env mode (GL_MODULATE default; the engine also selects GL_DECAL).
static GLint g_tex_env_mode = GL_MODULATE;

// GROUP 4 lighting state — consumed ONLY by the skinning shader (see glLightfv). ES1 defaults.
static GLfloat g_light0_ambient[4]      = {0.0f, 0.0f, 0.0f, 1.0f};
static GLfloat g_light0_diffuse[4]      = {1.0f, 1.0f, 1.0f, 1.0f};
static GLfloat g_light_model_ambient[4] = {0.2f, 0.2f, 0.2f, 1.0f};

// The live GL_ARRAY_BUFFER / GL_ELEMENT_ARRAY_BUFFER bindings, mirrored from the GROUP 2b
// glBindBuffer wrapper. The draw path binds its own streaming buffers and must put these back:
// docs/rendering.md's passes assume their predecessors restored state, and a stale binding here
// would corrupt the NEXT chunk's glBufferData rather than this one's draw — a bug that would
// present nowhere near its cause.
static GLuint g_bound_array_buffer = 0;
static GLuint g_bound_element_buffer = 0;

// --- GL buffer-byte accounting (ROADMAP Phase M / M0) --------------------------------------
// The number that turns "GL buffer lifetime — Unknown" (web-port-memory-plan.md §M3) into a
// lever or a dead end: how many bytes of vertex/index data the app currently has resident in
// driver-side GL buffers, and how much churn the reload burst puts through create/delete.
// GL is main-thread-only (root CLAUDE.md convention #4), so no atomics. Deliberately NOT gated
// on EDEN_DIAGNOSTICS — same reasoning as MeshTiming_web.mm: it must read in build-rel, the
// build players actually run. `bytes` is what the app ASKED for via glBufferData; the driver
// rounds each name up to at least a page (16 KB on iOS), so RSS-minus-this is driver overhead.
static std::unordered_map<GLuint, size_t> g_glbuf_sizes;
static size_t   g_glbuf_bytes      = 0;
static size_t   g_glbuf_peakBytes  = 0;
static unsigned g_glbuf_count      = 0;   // live buffer names that have had glBufferData
static unsigned g_glbuf_peakCount  = 0;
static unsigned g_glbuf_creates    = 0;   // names handed out by glGenBuffers, cumulative
static unsigned g_glbuf_deletes    = 0;   // names passed to glDeleteBuffers, cumulative

static void eden_gl_buf_note_data(GLenum target, size_t size) {
    GLuint name = (target == GL_ELEMENT_ARRAY_BUFFER) ? g_bound_element_buffer
                                                      : g_bound_array_buffer;
    if (name == 0) return;   // client-memory path / no buffer bound
    auto it = g_glbuf_sizes.find(name);
    if (it == g_glbuf_sizes.end()) {
        g_glbuf_sizes.emplace(name, size);
        g_glbuf_bytes += size;
        ++g_glbuf_count;
    } else {
        g_glbuf_bytes = g_glbuf_bytes - it->second + size;   // glBufferData reallocates
        it->second = size;
    }
    if (g_glbuf_bytes > g_glbuf_peakBytes) g_glbuf_peakBytes = g_glbuf_bytes;
    if (g_glbuf_count > g_glbuf_peakCount) g_glbuf_peakCount = g_glbuf_count;
}

static void eden_gl_buf_note_delete(GLuint name) {
    if (name == 0) return;
    ++g_glbuf_deletes;
    auto it = g_glbuf_sizes.find(name);
    if (it == g_glbuf_sizes.end()) return;
    g_glbuf_bytes -= it->second;
    --g_glbuf_count;
    g_glbuf_sizes.erase(it);
}

// Viewport, mirrored from the glViewport wrapper so glGetIntegerv(GL_VIEWPORT) COULD answer it.
// WebGL2 *can* answer this natively, but the mirror is what serves the pre-context window and
// keeps the headless build's picking math non-degenerate. As of item #6 (dynamic drawable) this
// tracks the REAL, dynamic drawable size — kept for parity with the real GL state, but
// eden_gl_glGetIntegerv() deliberately does NOT answer GL_VIEWPORT from this anymore; see
// kPickViewport below for why.
GLint g_viewport[4] = {0, 0, 0, 0};  // shared with gl_context_web.cpp, see gl_shim_internal.h

// The FIXED size Util.mm's findWorldCoords (the touch/click -> world raycast, engine code, never
// touched) implicitly assumes GL_VIEWPORT reports: SCREEN_WIDTH*SCALE_WIDTH x
// SCREEN_HEIGHT*SCALE_HEIGHT = 568*2 x 320*2 (EAGLView_web.mm's establishScreenMetrics — both
// halves are fixed constants that must never change, the point space for the engine's projection
// and the texture/icon scale factor used all over Hud.mm/Menu.mm). findWorldCoords converts a
// touch's POINT-space position to what it treats as a PIXEL position with `IS_IPAD { mx*=
// SCALE_HEIGHT; my*=SCALE_WIDTH; }`, then feeds that straight to gluUnProject against whatever
// GL_VIEWPORT reports — gluUnProject only ever uses the viewport to normalize winX/winY into NDC
// (a fraction), so this is correct exactly when the reported viewport matches what that x2 already
// assumes. Before item #6 the real drawable WAS always exactly 1136x640 (SCREEN was always
// retina-doubled 1:1), so mirroring the real size in g_viewport was harmless — same number either
// way. Now that the drawable is CSS box x min(devicePixelRatio, dpr_cap) x render_scale (almost
// never literally 1136x640), mirroring the REAL size feeds findWorldCoords's fixed x2 the WRONG
// denominator, and the raycast lands off by however far the real drawable's size diverges from
// 1136x640 — this was the "taps land a little to the left" bug on mobile. eden-st.html's
// toEnginePoint() already normalizes a touch to the fixed 568x320 point space using the CSS box's
// REAL current size (getBoundingClientRect()), so the (touch-fraction-of-screen) ratio is already
// correct going in; answering the FIXED size here, not the real one, is what makes findWorldCoords's
// x2 reconstruct that same fraction on the way back out, independent of DPR/render_scale/CSS
// aspect. NOT yet browser/`node eden.js`-verified (written in a sandbox with no build tree) — do
// that before trusting this fully, per this port's own "measure, never extrapolate" rule.
//
// NO LONGER A COMPILE-TIME CONSTANT (audit D1/D4): the point space is now derived from the window
// aspect and a UI-scale setting rather than pinned to the iPhone-5 profile, so "the fixed size the
// engine's x2 assumes" is whatever SCREEN_*xSCALE_* currently is. src/seam/DisplayProfile_web.mm
// writes it through eden_gl_set_pick_viewport() every time it recomputes the metrics; the
// initialiser below is still the 568x320@2x profile, which is what the headless build and the
// pre-first-layout frames get. What must stay true is the invariant, not the number: this answers
// the POINT space, never the real drawable.
GLint kPickViewport[4] = {0, 0, 1136, 640};  // shared, see gl_shim_internal.h


} // namespace eden_gl_shim

using namespace eden_gl_shim;

extern "C" {

// ---- GROUP 1: matrix stack ----

void glMatrixMode(GLenum mode) { g_matrix_mode = mode; }

// The matrix every "write the current matrix" entry point below targets. GL_MATRIX_PALETTE_OES
// is the one mode with no stack — it addresses g_palette[glCurrentPaletteMatrixOES] instead —
// so it is resolved here rather than in active(), which must keep returning a real stack for
// glPushMatrix/glPopMatrix. (The engine never pushes/pops in palette mode; ES1 says that is an
// error, and Classes/Model.mm:2732-2773 only ever selects-and-loads.)
static Mat4& current_matrix() {
    if (g_matrix_mode == GL_MATRIX_PALETTE_OES) {
        int i = g_current_palette;
        if (i < 0 || i >= kMaxPaletteMatrices) i = 0;
        return g_palette[i];
    }
    return active().stack[active().top];
}

void glLoadIdentity(void) { mat4_identity(current_matrix()); }

void glLoadMatrixf(const GLfloat* m) {
    std::memcpy(current_matrix().m, m, sizeof(GLfloat) * 16);
}

void glMultMatrixf(const GLfloat* m) {
    Mat4 rhs;
    std::memcpy(rhs.m, m, sizeof(GLfloat) * 16);
    Mat4& top = current_matrix();
    mat4_multiply(top, top, rhs);
}

void glPushMatrix(void) {
    MatrixMode& mm = active();
    if (mm.top + 1 >= kMaxStackDepth) return; // TODO P2: NSLog-equivalent warning on overflow
    mm.stack[mm.top + 1] = mm.stack[mm.top];
    mm.top++;
}

void glPopMatrix(void) {
    MatrixMode& mm = active();
    if (mm.top == 0) return; // TODO P2: warn on underflow
    mm.top--;
}

void glOrthof(GLfloat left, GLfloat right, GLfloat bottom, GLfloat top, GLfloat near_, GLfloat far_) {
    Mat4 ortho;
    mat4_identity(ortho);
    ortho.m[0]  = 2.0f / (right - left);
    ortho.m[5]  = 2.0f / (top - bottom);
    ortho.m[10] = -2.0f / (far_ - near_);
    ortho.m[12] = -(right + left) / (right - left);
    ortho.m[13] = -(top + bottom) / (top - bottom);
    ortho.m[14] = -(far_ + near_) / (far_ - near_);
    Mat4& cur = current_matrix();
    mat4_multiply(cur, cur, ortho);
}

void glFrustumf(GLfloat left, GLfloat right, GLfloat bottom, GLfloat top, GLfloat near_, GLfloat far_) {
    // No known caller (see header) — implemented anyway for completeness/symmetry with glOrthof.
    Mat4 f;
    std::memset(f.m, 0, sizeof(f.m));
    f.m[0]  = (2.0f * near_) / (right - left);
    f.m[5]  = (2.0f * near_) / (top - bottom);
    f.m[8]  = (right + left) / (right - left);
    f.m[9]  = (top + bottom) / (top - bottom);
    f.m[10] = -(far_ + near_) / (far_ - near_);
    f.m[11] = -1.0f;
    f.m[14] = -(2.0f * far_ * near_) / (far_ - near_);
    Mat4& cur = current_matrix();
    mat4_multiply(cur, cur, f);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z) {
    Mat4 s;
    mat4_identity(s);
    s.m[0] = x; s.m[5] = y; s.m[10] = z;
    Mat4& cur = current_matrix();
    mat4_multiply(cur, cur, s);
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    Mat4 t;
    mat4_identity(t);
    t.m[12] = x; t.m[13] = y; t.m[14] = z;
    Mat4& cur = current_matrix();
    mat4_multiply(cur, cur, t);
}

void glRotatef(GLfloat angle_deg, GLfloat x, GLfloat y, GLfloat z) {
    float len = std::sqrt(x * x + y * y + z * z);
    if (len < 1e-6f) return;
    x /= len; y /= len; z /= len;
    float rad = angle_deg * 3.14159265358979323846f / 180.0f;
    float c = std::cos(rad), s = std::sin(rad), ic = 1.0f - c;
    Mat4 r;
    mat4_identity(r);
    r.m[0] = x * x * ic + c;       r.m[4] = x * y * ic - z * s;    r.m[8]  = x * z * ic + y * s;
    r.m[1] = y * x * ic + z * s;   r.m[5] = y * y * ic + c;        r.m[9]  = y * z * ic - x * s;
    r.m[2] = x * z * ic - y * s;   r.m[6] = y * z * ic + x * s;    r.m[10] = z * z * ic + c;
    Mat4& cur = current_matrix();
    mat4_multiply(cur, cur, r);
}

// ---- GROUP 2: client arrays (state tracking only — draw-path translation is TODO P2) ----

static int eden_gl_slot_for_client_state(GLenum array) {
    switch (array) {
        case GL_VERTEX_ARRAY:          return ATTR_POSITION;
        case GL_COLOR_ARRAY:           return ATTR_COLOR;
        case GL_TEXTURE_COORD_ARRAY:   return ATTR_TEXCOORD;
        case GL_NORMAL_ARRAY:          return ATTR_NORMAL;
        case GL_POINT_SIZE_ARRAY_OES:  return ATTR_POINTSIZE;
        case GL_MATRIX_INDEX_ARRAY_OES: return ATTR_MATRIXINDEX;
        case GL_WEIGHT_ARRAY_OES:      return ATTR_WEIGHT;
        default:                       return -1;
    }
}

void glEnableClientState(GLenum array) {
    int slot = eden_gl_slot_for_client_state(array);
    if (slot >= 0) g_arrays[slot].enabled = true;
}

void glDisableClientState(GLenum array) {
    int slot = eden_gl_slot_for_client_state(array);
    if (slot >= 0) g_arrays[slot].enabled = false;
}

// Latch {size,type,stride,pointer} AND the current GL_ARRAY_BUFFER binding — see ClientArray's
// comment for why the binding belongs to the array, not to the draw call.
static void eden_gl_set_array(int slot, GLint size, GLenum type, GLsizei stride,
                              const void* pointer) {
    ClientArray& a = g_arrays[slot];
    a.size = size; a.type = type; a.stride = stride; a.pointer = pointer;
    a.buffer = g_bound_array_buffer;
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const void* pointer) {
    eden_gl_set_array(ATTR_POSITION, size, type, stride, pointer);
}
void glColorPointer(GLint size, GLenum type, GLsizei stride, const void* pointer) {
    eden_gl_set_array(ATTR_COLOR, size, type, stride, pointer);
}
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* pointer) {
    eden_gl_set_array(ATTR_TEXCOORD, size, type, stride, pointer);
}
void glNormalPointer(GLenum type, GLsizei stride, const void* pointer) {
    eden_gl_set_array(ATTR_NORMAL, 3, type, stride, pointer);   // ES1: normals are always 3-component
}

// ---- GROUP 8: GL_OES_matrix_palette entry points (installed into CPVRTglesExt's function
// pointer table by src/seam/pvrt_matrix_palette.cpp) ----------------------------------------
void eden_gl_glCurrentPaletteMatrixOES(GLuint matrixpaletteindex) {
    if ((int)matrixpaletteindex >= kMaxPaletteMatrices) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            // Would mean a POD bone-batched for more than ES1's own 9-matrix minimum. Clamping
            // makes the affected bone follow the wrong matrix rather than corrupt memory; raise
            // kMaxPaletteMatrices (and u_palette's size in kSkinVertexShader) if this ever fires.
            std::fprintf(stderr, "[eden-gl] palette matrix index %u >= %d; clamping.\n",
                         matrixpaletteindex, kMaxPaletteMatrices);
        }
        g_current_palette = kMaxPaletteMatrices - 1;
        return;
    }
    g_current_palette = (int)matrixpaletteindex;
}

// Never called by this engine (Classes/Model.mm builds each bone matrix by hand and glLoadMatrixf's
// it). Implemented anyway so the function pointer is never NULL — the engine's own null checks are
// on the DATA (pMesh->sBoneIdx.pData), not on the pointers.
void eden_gl_glLoadPaletteFromModelViewMatrixOES(void) {
    for (int i = 0; i < kMaxPaletteMatrices; ++i) g_palette[i] = g_modelview.stack[g_modelview.top];
}

void eden_gl_glMatrixIndexPointerOES(GLint size, GLenum type, GLsizei stride,
                                     const GLvoid* pointer) {
    eden_gl_set_array(ATTR_MATRIXINDEX, size, type, stride, pointer);
}

void eden_gl_glWeightPointerOES(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer) {
    eden_gl_set_array(ATTR_WEIGHT, size, type, stride, pointer);
}

// ---- GROUP 3: fog — state capture; the draw path turns it into shader uniforms ----
void glFogf(GLenum pname, GLfloat param) {
    switch (pname) {
        case GL_FOG_MODE:    g_fog_mode = (GLenum)param; break;
        case GL_FOG_DENSITY: g_fog_density = param; break;
        case GL_FOG_START:   g_fog_start = param; break;
        case GL_FOG_END:     g_fog_end = param; break;
        default: break;
    }
}
void glFogfv(GLenum pname, const GLfloat* params) {
    if (!params) return;
    if (pname == GL_FOG_COLOR) {
        for (int i = 0; i < 4; ++i) g_fog_color[i] = params[i];
    } else {
        glFogf(pname, params[0]);
    }
}
void glFogx(GLenum pname, GLfixed param) {
    // ES1 fixed-point is 16.16. No known caller (see header), but converting is one line and
    // silently treating the raw integer as a float would be a nasty thing to leave behind.
    glFogf(pname, (GLfloat)param / 65536.0f);
}

// ---- GROUP 4: lighting (TODO P2 — two-light doors/golden-cube uniforms) ----
// GROUP 4 lighting is still unimplemented for the general draw path (doors + the golden cube's
// env-mapped specular render unlit — see the shader section). GL_LIGHT0's ambient/diffuse ARE
// tracked, because the SKINNED path uses them: Classes/Model.mm:2336-2351 sets up one light per
// frame for the creatures and darkens it at night, and creatures lit at a flat 1.0 would glow
// against the terrain. Defaults below are ES1's own for LIGHT0.
void glLightfv(GLenum light, GLenum pname, const GLfloat* params) {
    if (light != GL_LIGHT0 || !params) return;
    if (pname == GL_AMBIENT)      std::memcpy(g_light0_ambient, params, sizeof(GLfloat) * 4);
    else if (pname == GL_DIFFUSE) std::memcpy(g_light0_diffuse, params, sizeof(GLfloat) * 4);
    // GL_POSITION is deliberately ignored: the engine's only call loads (0,0,0,1) under an
    // identity modelview, i.e. a point light AT THE EYE, which the skinning shader hard-codes.
}
void glLightModelfv(GLenum pname, const GLfloat* params) {
    if (pname == GL_LIGHT_MODEL_AMBIENT && params)
        std::memcpy(g_light_model_ambient, params, sizeof(GLfloat) * 4);
}
void glMaterialf(GLenum /*face*/, GLenum /*pname*/, GLfloat /*param*/) {}
void glMaterialfv(GLenum /*face*/, GLenum /*pname*/, const GLfloat* /*params*/) {}

// ---- GROUP 5: texture env ----
void glTexEnvi(GLenum target, GLenum pname, GLint param) {
    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE) g_tex_env_mode = param;
    // GL_POINT_SPRITE_OES/GL_COORD_REPLACE_OES also arrive here (SpecialEffects.mm). WebGL2
    // point rendering ALWAYS behaves as if coord-replace were on (gl_PointCoord is always
    // available and the shader below uses it for GL_POINTS), so there is nothing to record.
}

// ---- GROUP 6: fixed state ----
void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
    g_current_color[0] = red; g_current_color[1] = green;
    g_current_color[2] = blue; g_current_color[3] = alpha;
}
void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha) {
    g_current_color[0] = red / 255.0f; g_current_color[1] = green / 255.0f;
    g_current_color[2] = blue / 255.0f; g_current_color[3] = alpha / 255.0f;
}
void glAlphaFunc(GLenum func, GLclampf ref) { g_alpha_func = func; g_alpha_ref = ref; }
void glShadeModel(GLenum /*mode*/) {} // GL_FLAT vs GL_SMOOTH — engine bakes colors per-vertex
                                       // already (docs/rendering.md), likely a no-op forever.

// glGetFloatv: real matrix readback for Camera.mm/Model.mm (see header GROUP 6 note). Only
// the 3 matrix pnames are handled — anything else falls through to <GLES2/gl2.h>'s glGetFloatv
// (linked separately; NOT redefined here to avoid a duplicate-symbol clash — see gl.h note).
// This function is intentionally NOT named glGetFloatv to avoid colliding with the real ES2
// entry point; the shim's actual glGetFloatv interception (routing matrix pnames here before
// falling back to the real one) is TODO P2, needs a live GL context to test the fallback path.
void eden_gl_shim_get_matrix(GLenum pname, GLfloat* out) {
    const Mat4* src = nullptr;
    if (pname == GL_MODELVIEW_MATRIX)  src = &g_modelview.stack[g_modelview.top];
    if (pname == GL_PROJECTION_MATRIX) src = &g_projection.stack[g_projection.top];
    if (pname == GL_TEXTURE_MATRIX)    src = &g_texture.stack[g_texture.top];
    if (src) std::memcpy(out, src->m, sizeof(GLfloat) * 16);
}

// ---- GROUP 7: occlusion query EXT — safe no-ops (see header rationale) ----
void glBeginQueryEXT(GLenum /*target*/, GLuint /*id*/) {}
void glEndQueryEXT(GLenum /*target*/) {}
void glGetQueryObjectivEXT(GLuint /*id*/, GLenum pname, GLint* params) {
    // Report "result available, value 0" so TerrainChunk.mm's polling loop (isTesting,
    // disabled feature per docs/rendering.md) never spins.
    if (params) *params = 0;
    (void)pname;
}
void glGenQueriesEXT(GLsizei n, GLuint* ids) {
    for (GLsizei i = 0; i < n; ++i) ids[i] = 0;
}

// ---------------------------------------------------------------------------------------
// GROUP 8 — Point sprites (Classes/SpecialEffects.mm particles). See gl_es1_shim.h GROUP 8
// for why these are here and why the header's exclusion list needed correcting.
//
// State is CAPTURED, not discarded, even though nothing consumes it yet: Stage P2's shader
// needs the attenuation coefficients and the size clamp to reproduce ES1 point behavior
// (gl_PointSize = size / sqrt(a + b*d + c*d^2), clamped), and the size-array pointer becomes
// an ordinary vertex attribute. Throwing the values away here would mean rediscovering what
// the engine sets, from the engine, at P2 — this way the draw-path translation can just read
// them. Same approach as the matrix stack and color state, which are already captured.
// ---------------------------------------------------------------------------------------
GLfloat g_pointSizeMin = 0.0f;
GLfloat g_pointSizeMax = 1.0f;
GLfloat g_pointFadeThresholdSize = 1.0f;
GLfloat g_pointDistanceAttenuation[3] = {1.0f, 0.0f, 0.0f};   // ES1 default: constant, no falloff

const GLvoid* g_pointSizeArrayPointer = 0;
GLenum g_pointSizeArrayType = 0;
GLsizei g_pointSizeArrayStride = 0;

void glPointParameterf(GLenum pname, GLfloat param) {
    switch (pname) {
        case GL_POINT_SIZE_MIN:          g_pointSizeMin = param; break;
        case GL_POINT_SIZE_MAX:          g_pointSizeMax = param; break;
        case GL_POINT_FADE_THRESHOLD_SIZE: g_pointFadeThresholdSize = param; break;
        default: break;
    }
}

void glPointParameterfv(GLenum pname, const GLfloat* params) {
    if (!params) return;
    if (pname == GL_POINT_DISTANCE_ATTENUATION) {
        g_pointDistanceAttenuation[0] = params[0];
        g_pointDistanceAttenuation[1] = params[1];
        g_pointDistanceAttenuation[2] = params[2];
    } else {
        // The scalar parameters are also legal through the vector entry point.
        glPointParameterf(pname, params[0]);
    }
}

void glPointSizePointerOES(GLenum type, GLsizei stride, const GLvoid* pointer) {
    g_pointSizeArrayType = type;
    g_pointSizeArrayStride = stride;
    g_pointSizeArrayPointer = pointer;
    // Also latched as an ordinary client array (ATTR_POINTSIZE): in WebGL2 a per-vertex point
    // size is just a vertex attribute the shader writes to gl_PointSize, which is exactly what
    // the header predicted this would become.
    eden_gl_set_array(ATTR_POINTSIZE, 1, type, stride, pointer);
}

// ---- GROUP 2b: context guard over the passthrough surface -------------------------------
//
// Why this exists at all: Graphics::initGraphics() runs real GL during World::World(), before
// any context is created (in node: before any context CAN be created). Full rationale, and why
// headless-gl was rejected, is in gl_es1_shim.h's GROUP 2b comment.
//
// Each wrapper is "forward if live, else no-op". The predicate is the live Emscripten context
// handle, so the guard opens by itself the moment Stage P2's EAGLView_web.mm makes a context
// current — nothing here needs revisiting then.
//
// eden_gl_have_context()/eden_gl_context_is_lost() themselves are DEFINED in
// gl_context_web.cpp (Phase N Stage 0.2 split) — they read g_ctx/g_context_lost, which are
// Emscripten-context state — but declared in gl_es1_shim.h, so every guard below calls them
// exactly as before the split.

// One warning per process, on the first guarded call — a headless run should SAY it is drawing
// into the void. Silence here would be exactly the "swallow the error" failure mode that would
// hide a genuinely missing context in the browser build later.
static void eden_gl_warn_once(void) {
    static bool warned = false;
    if (warned) return;
    warned = true;
    std::fprintf(stderr,
        "[eden-gl] no WebGL context current — GL calls are being no-oped (headless mode).\n"
        "[eden-gl] expected under `node eden.js` and before EAGLView_web's context setup (TODO P2);\n"
        "[eden-gl] if you see this in a browser AFTER P2 lands, the context is genuinely missing.\n");
}

// Fake object names for the headless path. Must be nonzero: TerrainChunk.mm and Graphics.mm
// both treat 0 as "no buffer yet" and would re-generate forever otherwise. Buffers and textures
// share one counter — harmless, they are separate namespaces in real GL and nothing here is
// ever handed back to a real driver.
static GLuint g_fake_object_name = 0;

// ---- Draw-path call accounting (audit item #4) -------------------------------------------
// Not debug decoration: the whole point of the dirty-tracking below is a call-count reduction,
// and the port's standing rule is "measure, never extrapolate" (web/CLAUDE.md). `issued` counts
// the setup calls the shim actually made this frame; `elided` counts the ones the caches skipped
// — i.e. what the pre-item-#4 shim would have made on top of `issued`. Both are plain integer
// increments in wasm memory (no GL, no JS crossing), so they can stay in the shipped build; they
// are what `eden_gl_stat()` reports to the console.
namespace {
struct DrawStats {
    long draws  = 0;
    long issued = 0;
    long elided = 0;
};
DrawStats g_stats;      // accumulating, current frame
DrawStats g_statsLast;  // last completed frame, what eden_gl_stat() reports

inline void stat_issued(long n = 1) { g_stats.issued += n; }
inline void stat_elided(long n = 1) { g_stats.elided += n; }

void eden_gl_stats_flush_frame() {
    g_statsLast = g_stats;
    g_stats = DrawStats();
}
} // namespace

// The frame boundary for the accounting above. Called from the seam's own drawFrame
// (src/seam/EdenViewController_web.cpp) rather than inferred from glClear, because glClear is NOT
// a reliable frame marker in this engine: Graphics::prepareMenu clears the COLOR bit, but
// prepareScene does not (the sky is geometry that covers the frame), so a colour-bit boundary
// froze these counters the moment the game left the menu — measured, in a live browser session.
extern "C" EDEN_GL_EXPORT
void eden_gl_stats_frame_boundary(void) { eden_gl_stats_flush_frame(); }

// which: 0=draw calls, 1=setup GL calls issued, 2=setup GL calls elided by the caches,
// 3=issued+elided (what the un-cached shim would have made). Per LAST completed frame.
extern "C" EDEN_GL_EXPORT
int eden_gl_stat(int which) {
    switch (which) {
        case 0: return (int)g_statsLast.draws;
        case 1: return (int)g_statsLast.issued;
        case 2: return (int)g_statsLast.elided;
        case 3: return (int)(g_statsLast.issued + g_statsLast.elided);
        default: return 0;
    }
}

// ---- P2 debug instrumentation (single-threaded browser bring-up) ------------------------
// Counts draw traffic per frame and dumps a one-line summary for the first few frames, so a
// black canvas can be diagnosed WITHOUT a GL debugger: "draws happening but every one textured
// (and Texture2D is excluded, so those sample black)" looks completely different here from "no
// draws at all" or "draws + a GL error". Frame boundary = glClear(GL_COLOR_BUFFER_BIT), which is
// exactly where Graphics::prepareMenu/prepareScene begin a frame. Self-limiting to the first few
// frames and gated so glGetError only stalls the pipeline during them.
// TODO P2: delete alongside EdenViewController_web::drawFrame()'s [eden-p1] tick probe once a
// rendered frame is its own proof.
namespace {
struct FrameDbg {
    int  frame          = 0;
    int  drawArrays     = 0;
    int  drawElements   = 0;
    long verts          = 0;
    long indices        = 0;
    int  texturedDraws  = 0;
    unsigned glErr      = 0;
    float clearColor[4] = {-1.f, -1.f, -1.f, -1.f};
};
FrameDbg g_fdbg;
bool     g_dbg_lastDrawTextured = false;
const int kDbgFrames = 5;

// Q7/C8: both functions below are gated on EDEN_DIAGNOSTICS. Undefined, eden_gl_dbg_active()
// always answers false, which — since it is the sole guard around every fprintf/glGetError()
// call site below — removes the per-draw pipeline stall entirely from a build meant to be played,
// not just the printed noise.
void eden_gl_dbg_flush_frame() {
#ifdef EDEN_DIAGNOSTICS
    if (g_fdbg.frame >= kDbgFrames) return;
    std::fprintf(stderr,
        "[eden-gl-dbg] frame %d: clear=(%.2f,%.2f,%.2f,%.2f) drawArrays=%d(verts=%ld) "
        "drawElements=%d(idx=%ld) textured=%d/%d glErr=0x%x ctx=%d setup=%ld/%ld\n",
        g_fdbg.frame, g_fdbg.clearColor[0], g_fdbg.clearColor[1], g_fdbg.clearColor[2],
        g_fdbg.clearColor[3], g_fdbg.drawArrays, g_fdbg.verts, g_fdbg.drawElements,
        g_fdbg.indices, g_fdbg.texturedDraws, g_fdbg.drawArrays + g_fdbg.drawElements,
        g_fdbg.glErr, eden_gl_have_context(),
        // setup=issued/total: the item-#4 dirty-tracking ratio. Reads g_statsLast because
        // eden_gl_stats_frame_boundary() (called from drawFrame, i.e. BEFORE the render that
        // reaches this glClear) has already rotated the counters.
        g_statsLast.issued, g_statsLast.issued + g_statsLast.elided);
    g_fdbg.frame++;
    g_fdbg.drawArrays = g_fdbg.drawElements = g_fdbg.texturedDraws = 0;
    g_fdbg.verts = g_fdbg.indices = 0;
    g_fdbg.glErr = 0;
#endif
}
bool eden_gl_dbg_active() {
#ifdef EDEN_DIAGNOSTICS
    return g_fdbg.frame < kDbgFrames;
#else
    return false;
#endif
}
} // namespace

void eden_gl_glGenBuffers(GLsizei n, GLuint* buffers) {
    if (n > 0) g_glbuf_creates += (unsigned)n;   // M0 buffer-churn accounting
    if (eden_gl_have_context()) { glGenBuffers(n, buffers); return; }
    eden_gl_warn_once();
    for (GLsizei i = 0; i < n; ++i) buffers[i] = ++g_fake_object_name;
}

void eden_gl_glGenTextures(GLsizei n, GLuint* textures) {
    if (eden_gl_have_context()) { glGenTextures(n, textures); return; }
    eden_gl_warn_once();
    for (GLsizei i = 0; i < n; ++i) textures[i] = ++g_fake_object_name;
}

#define EDEN_GL_GUARD_VOID(name, params, args)      \
    void eden_gl_##name params {                    \
        if (eden_gl_have_context()) { name args; return; } \
        eden_gl_warn_once();                        \
    }

EDEN_GL_GUARD_VOID(glDeleteTextures, (GLsizei n, const GLuint* t), (n, t))
EDEN_GL_GUARD_VOID(glBufferSubData, (GLenum t, GLintptr o, GLsizeiptr s, const void* d), (t, o, s, d))

// glBufferData: not a plain passthrough — M0 (web-port-memory-plan.md) needs the resident
// byte count. It reallocates the store for the buffer currently bound to `target`, so this is
// where the app's driver-side geometry footprint is set.
void eden_gl_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    eden_gl_buf_note_data(target, size > 0 ? (size_t)size : 0u);
    if (eden_gl_have_context()) { glBufferData(target, size, data, usage); return; }
    eden_gl_warn_once();
}
EDEN_GL_GUARD_VOID(glBindTexture, (GLenum target, GLuint texture), (target, texture))
EDEN_GL_GUARD_VOID(glBlendFunc, (GLenum s, GLenum d), (s, d))
EDEN_GL_GUARD_VOID(glDepthMask, (GLboolean flag), (flag))
EDEN_GL_GUARD_VOID(glLineWidth, (GLfloat width), (width))
EDEN_GL_GUARD_VOID(glPolygonOffset, (GLfloat factor, GLfloat units), (factor, units))
EDEN_GL_GUARD_VOID(glHint, (GLenum target, GLenum mode), (target, mode))

#undef EDEN_GL_GUARD_VOID

// glDeleteBuffers: not a plain passthrough, because GL detaches a deleted buffer from EVERY binding
// point in the current context — the ARRAY_BUFFER binding AND any vertex attribute array that was
// specified against it, which both silently revert to 0. The item-#4 attribute cache would
// otherwise keep claiming "slot 0 already points at buffer 7, offset 12" after buffer 7 was
// deleted, skip the glVertexAttribPointer, and draw from a detached attribute. This is not
// hypothetical: TerrainChunk.mm deletes and regenerates its per-chunk VBOs as chunks stream
// (TerrainChunk.mm:1465-1469 and three more sites), and glGenBuffers recycles names.
static void eden_gl_forget_deleted_buffers(GLsizei n, const GLuint* buffers);

void eden_gl_glDeleteBuffers(GLsizei n, const GLuint* buffers) {
    eden_gl_forget_deleted_buffers(n, buffers);
    if (eden_gl_have_context()) { glDeleteBuffers(n, buffers); return; }
    eden_gl_warn_once();
}

// glTexParameteri / glTexImage2D: NOT plain passthroughs, unlike the macro group above.
//
// GL_GENERATE_MIPMAP is an ES1 texture PARAMETER (set before glTexImage2D, mipmaps built
// implicitly on upload); GLES2/WebGL2 deleted it from glTexParameter's enum set entirely —
// passing it raises GL_INVALID_ENUM (found by an actual browser run, Pass 12: every real
// texture upload in Texture2D_web.mm's initData hit this, hence "canvas still black" even
// after the asset-pipeline/locateFile fixes). The GLES2-shaped replacement is a NEW function,
// glGenerateMipmap(target), called AFTER the base-level image is uploaded. Texture2D.mm's own
// call order (bind -> TexParameteri(...GENERATE_MIPMAP...) -> TexImage2D) is preserved exactly
// (CLAUDE.md: no engine edits) — this shim just defers the actual mipmap generation to the
// point GLES2 requires it, tracked as a one-shot flag rather than per-texture state, because
// every real call site sets-then-immediately-uploads on the same bound texture (grep-verified:
// Texture2D.mm's initData is the only glTexImage2D caller in the whole engine).
namespace {
bool g_pending_generate_mipmap = false;
}
void eden_gl_glTexParameteri(GLenum target, GLenum pname, GLint param) {
    if (pname == GL_GENERATE_MIPMAP) {
        g_pending_generate_mipmap = (param != 0);
        return; // no GLES2 equivalent enum to forward — see comment above
    }
    if (eden_gl_have_context()) { glTexParameteri(target, pname, param); return; }
    eden_gl_warn_once();
}
void eden_gl_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                          GLsizei height, GLint border, GLenum format, GLenum type,
                          const void* pixels) {
    if (eden_gl_have_context()) {
        glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
        if (g_pending_generate_mipmap && level == 0) glGenerateMipmap(target);
        g_pending_generate_mipmap = false;
        return;
    }
    eden_gl_warn_once();
    g_pending_generate_mipmap = false;
}

// glClear / glClearColor: guarded like the macro group above, but also the P2 debug frame
// boundary (glClear with the color bit) and clear-color capture. See FrameDbg above.
void eden_gl_glClear(GLbitfield mask) {
    if (mask & GL_COLOR_BUFFER_BIT) eden_gl_dbg_flush_frame();
    if (eden_gl_have_context()) { glClear(mask); return; }
    eden_gl_warn_once();
}

void eden_gl_glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
    g_fdbg.clearColor[0] = r; g_fdbg.clearColor[1] = g;
    g_fdbg.clearColor[2] = b; g_fdbg.clearColor[3] = a;
    if (eden_gl_have_context()) { glClearColor(r, g, b, a); return; }
    eden_gl_warn_once();
}

// --- glEnable/glDisable: the ES1-only caps must NOT reach WebGL ---------------------------
//
// This is not a nicety. GL_TEXTURE_2D / GL_FOG / GL_LIGHTING / GL_LIGHT0 / GL_LIGHT1 /
// GL_ALPHA_TEST / GL_POINT_SPRITE_OES are all fixed-function caps that GLES2 deleted, so
// forwarding them raises GL_INVALID_ENUM per call — every frame, from Graphics.mm's state
// helpers, drowning any real error the engine might raise. They are captured as shader
// uniforms instead (that IS the fixed-function emulation). Everything else — GL_BLEND,
// GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_POLYGON_OFFSET_FILL, GL_DITHER,
// GL_STENCIL_TEST — is a real WebGL cap and forwards unchanged.
static bool eden_gl_capture_cap(GLenum cap, bool value) {
    switch (cap) {
        case GL_TEXTURE_2D:        g_texture2d_enabled = value;     return true;
        case GL_FOG:               g_fog_enabled = value;           return true;
        case GL_ALPHA_TEST:        g_alpha_test_enabled = value;    return true;
        case GL_LIGHTING:          g_lighting_enabled = value;      return true;
        case GL_POINT_SPRITE_OES:  g_point_sprite_enabled = value;  return true;
        case GL_MATRIX_PALETTE_OES: g_matrix_palette_enabled = value; return true;
        case GL_LIGHT0:
        case GL_LIGHT1:
            // Per-light enables. GROUP 4's lighting model itself is still unimplemented
            // (TODO P2b: doors + the golden cube's env-mapped specular are the only users,
            // docs/rendering.md), so these are swallowed rather than tracked — recording a bit
            // nothing reads would only look like the feature works.
            return true;
        case GL_COLOR_MATERIAL:
        case GL_NORMALIZE:
        case GL_RESCALE_NORMAL:
        case GL_POINT_SIZE_ARRAY_OES:
            return true;   // ES1-only, no WebGL equivalent, nothing downstream needs them
        default:
            return false;  // a genuine WebGL cap — forward it
    }
}

void eden_gl_glEnable(GLenum cap) {
    if (eden_gl_capture_cap(cap, true)) return;
    if (eden_gl_have_context()) { glEnable(cap); return; }
    eden_gl_warn_once();
}

void eden_gl_glDisable(GLenum cap) {
    if (eden_gl_capture_cap(cap, false)) return;
    if (eden_gl_have_context()) { glDisable(cap); return; }
    eden_gl_warn_once();
}

// --- glBindBuffer / glViewport: forward AND mirror ----------------------------------------
void eden_gl_glBindBuffer(GLenum target, GLuint buffer) {
    if (target == GL_ARRAY_BUFFER)              g_bound_array_buffer = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) g_bound_element_buffer = buffer;
    if (eden_gl_have_context()) { glBindBuffer(target, buffer); return; }
    eden_gl_warn_once();
}

void eden_gl_glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    g_viewport[0] = x; g_viewport[1] = y; g_viewport[2] = width; g_viewport[3] = height;
    if (eden_gl_have_context()) { glViewport(x, y, width, height); return; }
    eden_gl_warn_once();
}

void eden_gl_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                          GLenum type, void* pixels) {
    if (eden_gl_have_context()) { glReadPixels(x, y, width, height, format, type, pixels); return; }
    eden_gl_warn_once();
    // Caller (Util.mm's screenshot path) owns the buffer and reads it back unconditionally —
    // zero-fill rather than leave it uninitialized. RGBA/UNSIGNED_BYTE is the only combination
    // the engine asks for; anything else would need a real size table, hence the guard.
    if (pixels && format == GL_RGBA && type == GL_UNSIGNED_BYTE && width > 0 && height > 0) {
        std::memset(pixels, 0, (size_t)width * (size_t)height * 4u);
    }
}

void eden_gl_glGetIntegerv(GLenum pname, GLint* params) {
    if (!params) return;
    // GL_VIEWPORT is answered from the FIXED kPickViewport, in BOTH modes, not the real dynamic
    // g_viewport mirror — see kPickViewport's comment above (near g_viewport's declaration) for
    // why: Util.mm's findWorldCoords (the picking raycast) assumes this always reports the
    // engine's fixed retina-doubled point space (1136x640), and answering the real, item-#6-dynamic
    // drawable size here is exactly what caused mobile taps to land off-target.
    if (pname == GL_VIEWPORT) {
        for (int i = 0; i < 4; ++i) params[i] = kPickViewport[i];
        return;
    }
    if (eden_gl_have_context()) { glGetIntegerv(pname, params); return; }
    eden_gl_warn_once();
    params[0] = 0;
    if (pname == GL_SCISSOR_BOX) { params[1] = params[2] = params[3] = 0; }
}

void eden_gl_glGetFloatv(GLenum pname, GLfloat* params) {
    if (!params) return;
    // The three matrix pnames are the shim's own state — WebGL2 cannot answer them at all.
    if (pname == GL_MODELVIEW_MATRIX || pname == GL_PROJECTION_MATRIX ||
        pname == GL_TEXTURE_MATRIX) {
        eden_gl_shim_get_matrix(pname, params);
        return;
    }
    if (eden_gl_have_context()) { glGetFloatv(pname, params); return; }
    eden_gl_warn_once();
    params[0] = 0.0f;
}

GLenum eden_gl_glGetError(void) {
    if (eden_gl_have_context()) return glGetError();
    return GL_NO_ERROR;   // no context, no commands, no errors — don't invent one
}

const GLubyte* eden_gl_glGetString(GLenum name) {
    // GL_EXTENSIONS is ANSWERED, not forwarded. WebGL's own list never contains
    // GL_OES_matrix_palette, and Classes/Model.mm:1660 makes that string the gate on loading the
    // creature PODs at all — the shim emulates the extension (GROUP 8), so it must also advertise
    // it or LoadModels() early-returns and no creature ever exists. Everything WebGL really does
    // support is still listed, appended after ours, so any other probe still gets the truth.
    if (name == GL_EXTENSIONS) {
        static std::string exts = "GL_OES_matrix_palette";
        // Cached only once the real list is available — caching a context-less answer would
        // permanently hide WebGL's own extensions from any later probe.
        static bool merged = false;
        if (!merged && eden_gl_have_context()) {
            merged = true;
            const GLubyte* real = glGetString(GL_EXTENSIONS);
            if (real) { exts += " "; exts += (const char*)real; }
        }
        return (const GLubyte*)exts.c_str();
    }
    if (eden_gl_have_context()) return glGetString(name);
    eden_gl_warn_once();
    // Empty string, never NULL: callers strstr() extension lists and a NULL would crash them.
    return (const GLubyte*)"";
}

// =========================================================================================
// GROUP 2d — THE DRAW PATH: ES1 client arrays + fixed-function state -> attributes + shader
//
// This is what Stage P2 actually owed (PORT-STATUS Pass 8: "what P2 still owes is the
// draw-path translation"). One program covers every pass the engine runs, because the engine's
// own fixed-function usage is narrow (docs/rendering.md "Fixed constraints"): baked per-vertex
// colors, one texture unit, linear fog, a texture matrix for the atlas trick, and point sprites
// for particles. Branch-per-feature via uniforms rather than a shader permutation cache — at
// this call volume the uniform cost is invisible next to the per-chunk draw overhead, and one
// shader is one place to get the state pairing right.
//
// NOT covered yet, and honestly so: GROUP 4 lighting (GL_LIGHT0/GL_LIGHT1). Its only users are
// doors and the golden cube's env-mapped specular; they will render unlit until P2b. Nothing
// else in the frame uses GL_LIGHTING.
// =========================================================================================

static const char* kVertexShader =
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_mv;\n"
    "uniform mat4 u_texmat;\n"
    "uniform vec4 u_color;\n"
    "uniform float u_useColorArray;\n"
    "uniform float u_usePointSizeArray;\n"
    "uniform float u_pointSize;\n"
    "uniform vec3 u_pointAtten;\n"
    "EDEN_ATTR vec4 a_position;\n"
    "EDEN_ATTR vec4 a_color;\n"
    "EDEN_ATTR vec2 a_texcoord;\n"
    "EDEN_ATTR vec3 a_normal;\n"
    "EDEN_ATTR float a_pointsize;\n"
    "EDEN_VARY vec4 v_color;\n"
    "EDEN_VARY vec2 v_texcoord;\n"
    "EDEN_VARY float v_eyedist;\n"
    "void main() {\n"
    "  gl_Position = u_mvp * a_position;\n"
    "  vec4 eye = u_mv * a_position;\n"
    "  v_eyedist = length(eye.xyz);\n"
    // ES1: the per-vertex color array wins when enabled, else the last glColor4f applies.
    "  v_color = mix(u_color, a_color, u_useColorArray);\n"
    // The texture matrix is the atlas trick (glScalef(1, 1/32, 1)) and water/lava UV scroll.
    "  v_texcoord = (u_texmat * vec4(a_texcoord, 0.0, 1.0)).xy;\n"
    // ES1 point attenuation: size / sqrt(a + b*d + c*d^2), then clamped to the min/max range.
    "  float ps = mix(u_pointSize, a_pointsize, u_usePointSizeArray);\n"
    "  float d = v_eyedist;\n"
    "  float att = sqrt(max(u_pointAtten.x + u_pointAtten.y*d + u_pointAtten.z*d*d, 1e-6));\n"
    "  gl_PointSize = ps / att;\n"
    "  a_normal;\n"   // declared so GROUP 4 has its attribute wired; unused until P2b lighting
    "}\n";

static const char* kFragmentShader =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_useTexture;\n"
    "uniform float u_texEnvDecal;\n"
    "uniform float u_pointSprite;\n"
    "uniform float u_fogEnabled;\n"
    "uniform vec4 u_fogColor;\n"
    "uniform float u_fogStart;\n"
    "uniform float u_fogEnd;\n"
    "uniform float u_alphaTestEnabled;\n"
    "uniform float u_alphaRef;\n"
    "EDEN_VARY vec4 v_color;\n"
    "EDEN_VARY vec2 v_texcoord;\n"
    "EDEN_VARY float v_eyedist;\n"
    "void main() {\n"
    "  vec4 c = v_color;\n"
    "  if (u_useTexture > 0.5) {\n"
    // Point sprites take their UV from gl_PointCoord — ES1's GL_COORD_REPLACE_OES, which WebGL
    // provides unconditionally.
    "    vec2 uv = mix(v_texcoord, gl_PointCoord, u_pointSprite);\n"
    "    vec4 t = EDEN_TEX2D(u_tex, uv);\n"
    "    vec4 modulated = c * t;\n"
    "    vec4 decal = vec4(mix(c.rgb, t.rgb, t.a), c.a);\n"
    "    c = mix(modulated, decal, u_texEnvDecal);\n"
    "  }\n"
    // GLES2 deleted the alpha test; `discard` is the only equivalent. See the uniform setup for
    // why this is gated on the comparison function, not just on GL_ALPHA_TEST being enabled.
    "  if (u_alphaTestEnabled > 0.5 && c.a <= u_alphaRef) discard;\n"
    "  if (u_fogEnabled > 0.5) {\n"
    "    float f = clamp((u_fogEnd - v_eyedist) / max(u_fogEnd - u_fogStart, 1e-6), 0.0, 1.0);\n"
    "    c.rgb = mix(u_fogColor.rgb, c.rgb, f);\n"
    "  }\n"
    "  EDEN_FRAGCOLOR = c;\n"
    "}\n";

// =========================================================================================
// GROUP 8 — the skinning program (GL_OES_matrix_palette emulation, pass 27).
//
// WHY A SECOND PROGRAM RATHER THAN A BRANCH IN THE ONE ABOVE: blending the palette means
// indexing `u_palette[]` by a value that arrives in a vertex ATTRIBUTE. GLSL ES 1.00 (which the
// shader above is written in — `attribute`/`varying`/`gl_FragColor`) only permits uniform-array
// indexing by a "constant-index-expression", which an attribute is not; ES 3.00 permits it
// outright. WebGL2 happily runs programs of both GLSL versions side by side, so the skinned draw
// gets its own ES 3.00 program and the browser-verified main path is left exactly as it was.
//
// Coordinate spaces (this is the part that is easy to get subtly wrong): ES1's matrix palette
// REPLACES the modelview for skinned vertices. Classes/Model.mm loads each palette entry as
// `m_mView * m_mTransform * boneWorld`, where m_mView was read back from GL_MODELVIEW_MATRIX
// (so it already includes RenderModels' glScalef) — i.e. a palette matrix maps object space
// straight to EYE space. Hence `u_proj` here, not `u_mvp`, and the eye-space position for fog
// comes out of the blend for free.
static const char* kSkinVertexShader =
    "uniform mat4 u_proj;\n"
    "uniform mat4 u_palette[16];\n"
    "uniform mat4 u_texmat;\n"
    "uniform vec4 u_color;\n"
    "uniform int  u_boneCount;\n"
    "uniform vec3 u_lightAmbient;\n"
    "uniform vec3 u_lightDiffuse;\n"
    "in vec4 a_position;\n"
    "in vec2 a_texcoord;\n"
    "in vec3 a_normal;\n"
    "in vec4 a_matrixindex;\n"
    "in vec4 a_weight;\n"
    "out vec4 v_color;\n"
    "out vec2 v_texcoord;\n"
    "out float v_eyedist;\n"
    "void main() {\n"
    "  mat4 skin = mat4(0.0);\n"
    "  float wsum = 0.0;\n"
    // u_boneCount is the POD's own per-vertex influence count (sBoneWeight.n). It matters:
    // glVertexAttribPointer defaults the components a smaller array does not supply to
    // (_,0,0,1), so an unguarded 4-wide loop would read a phantom weight of 1.0 in .w.
    "  for (int b = 0; b < 4; ++b) {\n"
    "    if (b >= u_boneCount) break;\n"
    "    float w = a_weight[b];\n"
    "    int idx = clamp(int(a_matrixindex[b] + 0.5), 0, 15);\n"
    "    skin += u_palette[idx] * w;\n"
    "    wsum += w;\n"
    "  }\n"
    // Unweighted (or degenerate) vertices follow palette matrix 0 rigidly, which is what the
    // fixed-function pipeline does with an all-zero weight set.
    "  if (wsum <= 0.0001) skin = u_palette[0];\n"
    "  vec4 eye = skin * a_position;\n"
    "  gl_Position = u_proj * eye;\n"
    "  v_eyedist = length(eye.xyz);\n"
    "  vec3 n = normalize(mat3(skin) * a_normal);\n"
    // GL_LIGHT0 is a POINT light at the eye (Classes/Model.mm:2336 loads position (0,0,0,1) under
    // an identity modelview), so its direction at this vertex is simply -eye. GL_COLOR_MATERIAL is
    // on globally (Classes/Graphics.mm:183), which makes the material ambient AND diffuse both the
    // current glColor — that is why the whole lighting result is one scalar times u_color, and why
    // RenderModels' flash/on-fire/creature tinting still shows through.
    "  vec3 L = normalize(-eye.xyz);\n"
    "  float ndotl = max(dot(n, L), 0.0);\n"
    "  vec3 lit = u_lightAmbient + u_lightDiffuse * ndotl;\n"
    "  v_color = vec4(clamp(u_color.rgb * lit, 0.0, 1.0), u_color.a);\n"
    "  v_texcoord = (u_texmat * vec4(a_texcoord, 0.0, 1.0)).xy;\n"
    "}\n";

static const char* kSkinFragmentShader =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_useTexture;\n"
    "uniform float u_fogEnabled;\n"
    "uniform vec4 u_fogColor;\n"
    "uniform float u_fogStart;\n"
    "uniform float u_fogEnd;\n"
    "in vec4 v_color;\n"
    "in vec2 v_texcoord;\n"
    "in float v_eyedist;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  vec4 c = v_color;\n"
    "  if (u_useTexture > 0.5) c *= texture(u_tex, v_texcoord);\n"
    "  if (u_fogEnabled > 0.5) {\n"
    "    float f = clamp((u_fogEnd - v_eyedist) / max(u_fogEnd - u_fogStart, 1e-6), 0.0, 1.0);\n"
    "    c.rgb = mix(u_fogColor.rgb, c.rgb, f);\n"
    "  }\n"
    "  fragColor = c;\n"
    "}\n";

static GLuint g_program = 0;
static GLuint g_skin_program = 0;
static GLuint g_stream_vbo[ATTR_COUNT] = {0, 0, 0, 0, 0, 0, 0};
static GLuint g_stream_ibo = 0;

static struct {
    GLint mvp, mv, texmat, color, useColorArray, usePointSizeArray, pointSize, pointAtten;
    GLint tex, useTexture, texEnvDecal, pointSprite;
    GLint fogEnabled, fogColor, fogStart, fogEnd;
    GLint alphaTestEnabled, alphaRef;
} g_uni;

static struct {
    GLint proj, palette, texmat, color, boneCount, lightAmbient, lightDiffuse;
    GLint tex, useTexture, fogEnabled, fogColor, fogStart, fogEnd;
} g_skin_uni;

// =========================================================================================
// Redundancy caches — audit finding C2 / ROI item #4.
//
// THE PROBLEM, measured: every single draw used to issue glUseProgram + 17 glUniform* +
// glActiveTexture + a full 7-slot attribute re-specification + 2 restore binds — ~35-45 WebGL
// calls, each a wasm->JS crossing. TerrainChunk.mm draws SIX times per visible chunk (one per face
// direction) with identical state, so at a few hundred chunks that was ~50,000 crossings a frame
// before a single HUD quad.
//
// THE FIX: shadow every piece of state the draw path sets and issue only the delta. This is sound
// rather than a gamble because the shim is the ONLY writer of all of it — the programs, their
// uniforms and the vertex-attribute arrays are created here, written here, and nothing in
// Classes/ can touch them (the engine speaks ES1: glColor4f, glVertexPointer, glEnable(GL_FOG)…,
// all of which land in this file's software state, never in a GL object). The one piece of state
// the engine DOES share is the GL_ARRAY_BUFFER / GL_ELEMENT_ARRAY_BUFFER binding, which is why
// those two are tracked against the existing g_bound_*_buffer mirrors instead of being cached
// blindly (see eden_gl_restore_bindings).
//
// WHY NO VAOs, despite the audit naming them: a VAO would replace the per-chunk bind+3
// glVertexAttribPointer that survives the caching above — 4 calls per chunk, not per draw, once
// dirty-tracking is in. Against that: the ELEMENT_ARRAY_BUFFER binding is part of VAO state, and
// TerrainChunk.mm binds its own EBO *outside* any draw call, so a non-default VAO bound at that
// moment would capture the engine's bind into the wrong container — precisely the "corrupt the
// NEXT chunk, nowhere near the cause" failure eden_gl_restore_bindings exists to prevent. The
// tuple would also have to include each chunk's own VBO+EBO pair, i.e. hundreds of VAOs needing
// invalidation on glDeleteBuffers. Bad trade for ~3 calls/chunk; revisit only with a profile that
// says the remaining setup traffic still matters. (Measure it with eden_gl_stat().)
// =========================================================================================

static GLuint g_bound_program = 0;        // 0 = unknown/none, matches the post-link initial state
static GLenum g_active_texture = 0;       // 0 = unknown; GL's own default is GL_TEXTURE0

static inline bool eden_cache_f(float& slot, float v) {
    if (slot == v) return false;
    slot = v; return true;
}
static inline bool eden_cache_i(GLint& slot, GLint v) {
    if (slot == v) return false;
    slot = v; return true;
}
static inline bool eden_cache_fv(GLfloat* slot, const GLfloat* v, int n) {
    if (std::memcmp(slot, v, sizeof(GLfloat) * n) == 0) return false;
    std::memcpy(slot, v, sizeof(GLfloat) * n); return true;
}
static inline bool eden_cache_m(Mat4& slot, const Mat4& v) {
    if (std::memcmp(slot.m, v.m, sizeof(v.m)) == 0) return false;
    slot = v; return true;
}

// `valid` distinguishes "cached value happens to be 0" from "never uploaded to this program".
// Cleared whenever the program object is (re)created, since a fresh program's uniforms are zero
// but its sampler/flag defaults are not what any of these fields mean.
struct MainUniCache {
    bool    valid = false;
    Mat4    mvp, mv, texmat;
    GLfloat color[4], pointAtten[3], fogColor[4];
    float   useColorArray, usePointSizeArray, pointSize;
    float   useTexture, texEnvDecal, pointSprite;
    float   fogEnabled, fogStart, fogEnd, alphaTestEnabled, alphaRef;
};
struct SkinUniCache {
    bool    valid = false;
    Mat4    proj, texmat, palette[kMaxPaletteMatrices];
    GLfloat color[4], lightAmbient[3], lightDiffuse[3], fogColor[4];
    GLint   boneCount;
    float   useTexture, fogEnabled, fogStart, fogEnd;
};
static MainUniCache g_uniCache;
static SkinUniCache g_skinCache;

// The last vertex-attribute specification pushed to GL, per slot. The zero-initialised state is
// the truthful one for a fresh context: every attribute array starts DISABLED.
struct AttrCache {
    bool        enabled    = false;
    GLint       size       = 0;
    GLenum      type       = 0;
    GLboolean   normalized = GL_FALSE;
    GLsizei     stride     = 0;
    const void* pointer    = nullptr;
    GLuint      buffer     = 0;
};
static AttrCache g_attrCache[ATTR_COUNT];

// What the draw path currently has bound, so eden_gl_restore_bindings can skip a no-op restore.
static GLuint g_path_array_buffer = 0;
static bool   g_path_ebo_dirty    = false;

static inline void eden_gl_use_program(GLuint prog) {
    if (g_bound_program == prog) { stat_elided(); return; }
    glUseProgram(prog);
    g_bound_program = prog;
    stat_issued();
}

static inline void eden_gl_active_texture0(void) {
    if (g_active_texture == GL_TEXTURE0) { stat_elided(); return; }
    glActiveTexture(GL_TEXTURE0);
    g_active_texture = GL_TEXTURE0;
    stat_issued();
}

// See eden_gl_glDeleteBuffers above for why this exists. Cheap: n is 1 at every call site.
static void eden_gl_forget_deleted_buffers(GLsizei n, const GLuint* buffers) {
    if (!buffers) return;
    for (GLsizei i = 0; i < n; ++i) {
        const GLuint name = buffers[i];
        if (name == 0) continue;
        eden_gl_buf_note_delete(name);   // M0 buffer-byte accounting
        for (int slot = 0; slot < ATTR_COUNT; ++slot) {
            if (g_attrCache[slot].buffer == name) g_attrCache[slot] = AttrCache();
        }
        // The engine's own binding mirrors have to follow the same rule, or a later restore would
        // try to bind a deleted name (and eden_gl_restore_bindings would compare against it).
        if (g_bound_array_buffer == name)   g_bound_array_buffer = 0;
        if (g_bound_element_buffer == name) g_bound_element_buffer = 0;
        if (g_path_array_buffer == name)    g_path_array_buffer = 0;
    }
}

void eden_gl_shim_invalidate_gl_objects(void) {
    g_program = 0;
    g_skin_program = 0;
    g_stream_ibo = 0;
    for (int i = 0; i < ATTR_COUNT; ++i) g_stream_vbo[i] = 0;
    // Every cache above describes state inside objects that just went away (or inside a context
    // that did). Forgetting this is how a shim starts skipping uploads that never happened.
    g_bound_program = 0;
    g_active_texture = 0;
    g_uniCache = MainUniCache();
    g_skinCache = SkinUniCache();
    for (int i = 0; i < ATTR_COUNT; ++i) g_attrCache[i] = AttrCache();
    g_path_array_buffer = 0;
    g_path_ebo_dirty = false;
}

// Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md): the four shader sources above
// are SHARED between the WebGL2 backend and desktop GL core profile, and the two dialects
// disagree about spelling, not about semantics. The differences are handled by prepending a
// per-backend preamble rather than by forking the sources, per the plan's explicit instruction.
//
// WHY THE SOURCES SAY `EDEN_ATTR`/`EDEN_VARY`/`EDEN_TEX2D`/`EDEN_FRAGCOLOR` rather than the
// natural GLSL ES 1.00 spellings with a desktop preamble aliasing them: GLSL forbids #define'ing
// a name that starts with `gl_`, so `#define gl_FragColor <something>` is not available on the
// desktop side, and `attribute`/`varying` are reserved keywords in core profile (drivers vary in
// how sternly they enforce a #define over one). Neutral names put every alias on ground both
// dialects allow. On the ES path each alias expands to exactly the token that was there before
// this change, so the shader the browser compiles is textually unchanged apart from the
// preamble's #define lines.
//
// `es300` selects the GROUP 8 skinning pair's dialect (ES 3.00 / desktop 330 syntax: in/out/
// texture/an explicit fragment output) from the main pair's (ES 1.00 / aliased). Both map to
// `#version 330 core` on desktop, which is why the skinning shaders needed only their leading
// `#version 300 es` line lifted out into here.
//
// PHASE N STAGE 4.1 — THE SWITCH IS "IS THE CONTEXT AN ES ONE", NOT "IS THIS EMSCRIPTEN".
// It read `#if defined(__EMSCRIPTEN__)` until iOS arrived, and on iOS both halves of that
// equation change: the target is not Emscripten and the context IS OpenGL ES 3.0, so the desktop
// branch below would have handed a GLES driver `#version 330 core`, `in`/`out` at ES 1.00 and a
// user-declared fragment output — a compile failure in every shader the engine draws with, i.e. a
// black screen with a shader log in it. EDEN_GL_ES_DIALECT (native/CMakeLists.txt defines it on
// the iOS arm; Emscripten sets it here) names the property that actually decides the answer.
#if defined(__EMSCRIPTEN__) && !defined(EDEN_GL_ES_DIALECT)
#define EDEN_GL_ES_DIALECT 1
#endif

static const char* eden_gl_preamble(GLenum stage, bool es300) {
    const bool vertex = (stage == GL_VERTEX_SHADER);
#if defined(EDEN_GL_ES_DIALECT)
    if (es300) return "#version 300 es\n";
    // GLSL ES 1.00 is the default when no #version is given — keep it that way.
    return vertex ? "#define EDEN_ATTR attribute\n"
                    "#define EDEN_VARY varying\n"
                  : "#define EDEN_VARY varying\n"
                    "#define EDEN_TEX2D texture2D\n"
                    "#define EDEN_FRAGCOLOR gl_FragColor\n";
#else
    if (es300) return "#version 330 core\n";
    // Core profile: `attribute`/`varying` are gone, texture2D() folded into the overloaded
    // texture(), and there is no gl_FragColor — a user-declared output takes its place.
    return vertex ? "#version 330 core\n"
                    "#define EDEN_ATTR in\n"
                    "#define EDEN_VARY out\n"
                  : "#version 330 core\n"
                    "#define EDEN_VARY in\n"
                    "#define EDEN_TEX2D texture\n"
                    "out vec4 eden_fragColor;\n"
                    "#define EDEN_FRAGCOLOR eden_fragColor\n";
#endif
}

static GLuint eden_gl_compile(GLenum stage, const char* src, bool es300 = false) {
    GLuint sh = glCreateShader(stage);
    const char* parts[2] = { eden_gl_preamble(stage, es300), src };
    glShaderSource(sh, 2, parts, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
        std::fprintf(stderr, "[eden-gl] shader compile failed (%s):\n%s\n",
                     stage == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static bool eden_gl_ensure_program(void) {
    if (g_program) return true;

    GLuint vs = eden_gl_compile(GL_VERTEX_SHADER, kVertexShader);
    if (!vs) return false;
    GLuint fs = eden_gl_compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!fs) { glDeleteShader(vs); return false; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    // Attribute locations are BOUND, not queried, so they equal the ATTR_* slot indices — that
    // is what lets the array setup below iterate slots and use the index directly as the
    // attribute location, with no lookup table to drift out of sync.
    glBindAttribLocation(prog, ATTR_POSITION,  "a_position");
    glBindAttribLocation(prog, ATTR_COLOR,     "a_color");
    glBindAttribLocation(prog, ATTR_TEXCOORD,  "a_texcoord");
    glBindAttribLocation(prog, ATTR_NORMAL,    "a_normal");
    glBindAttribLocation(prog, ATTR_POINTSIZE, "a_pointsize");
    glBindAttribLocation(prog, ATTR_MATRIXINDEX, "a_matrixindex");
    glBindAttribLocation(prog, ATTR_WEIGHT,      "a_weight");
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
        std::fprintf(stderr, "[eden-gl] program link failed:\n%s\n", log);
        glDeleteProgram(prog);
        return false;
    }
    g_program = prog;
    g_uniCache = MainUniCache();   // fresh program object => nothing has been uploaded to it

#define EDEN_UNI(field, name) g_uni.field = glGetUniformLocation(g_program, name)
    EDEN_UNI(mvp, "u_mvp");                     EDEN_UNI(mv, "u_mv");
    EDEN_UNI(texmat, "u_texmat");               EDEN_UNI(color, "u_color");
    EDEN_UNI(useColorArray, "u_useColorArray"); EDEN_UNI(usePointSizeArray, "u_usePointSizeArray");
    EDEN_UNI(pointSize, "u_pointSize");         EDEN_UNI(pointAtten, "u_pointAtten");
    EDEN_UNI(tex, "u_tex");                     EDEN_UNI(useTexture, "u_useTexture");
    EDEN_UNI(texEnvDecal, "u_texEnvDecal");     EDEN_UNI(pointSprite, "u_pointSprite");
    EDEN_UNI(fogEnabled, "u_fogEnabled");       EDEN_UNI(fogColor, "u_fogColor");
    EDEN_UNI(fogStart, "u_fogStart");           EDEN_UNI(fogEnd, "u_fogEnd");
    EDEN_UNI(alphaTestEnabled, "u_alphaTestEnabled"); EDEN_UNI(alphaRef, "u_alphaRef");
#undef EDEN_UNI

    for (int i = 0; i < ATTR_COUNT; ++i) {
        if (!g_stream_vbo[i]) glGenBuffers(1, &g_stream_vbo[i]);
    }
    if (!g_stream_ibo) glGenBuffers(1, &g_stream_ibo);
    return true;
}

// GROUP 8's second program. Shares eden_gl_setup_attributes with the main one — that works only
// because both bind the SAME ATTR_* slot numbers to their attribute names, so the setup loop can
// keep using the slot index as the attribute location with no per-program lookup table.
static bool eden_gl_ensure_skin_program(void) {
    if (g_skin_program) return true;

    GLuint vs = eden_gl_compile(GL_VERTEX_SHADER, kSkinVertexShader, /*es300=*/true);
    if (!vs) return false;
    GLuint fs = eden_gl_compile(GL_FRAGMENT_SHADER, kSkinFragmentShader, /*es300=*/true);
    if (!fs) { glDeleteShader(vs); return false; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, ATTR_POSITION,    "a_position");
    glBindAttribLocation(prog, ATTR_COLOR,       "a_color");
    glBindAttribLocation(prog, ATTR_TEXCOORD,    "a_texcoord");
    glBindAttribLocation(prog, ATTR_NORMAL,      "a_normal");
    glBindAttribLocation(prog, ATTR_POINTSIZE,   "a_pointsize");
    glBindAttribLocation(prog, ATTR_MATRIXINDEX, "a_matrixindex");
    glBindAttribLocation(prog, ATTR_WEIGHT,      "a_weight");
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(prog, sizeof(log) - 1, nullptr, log);
        std::fprintf(stderr, "[eden-gl] skinning program link failed:\n%s\n", log);
        glDeleteProgram(prog);
        return false;
    }
    g_skin_program = prog;
    g_skinCache = SkinUniCache();  // as above — see eden_gl_ensure_program

#define EDEN_SKIN_UNI(field, name) g_skin_uni.field = glGetUniformLocation(g_skin_program, name)
    EDEN_SKIN_UNI(proj, "u_proj");                 EDEN_SKIN_UNI(palette, "u_palette[0]");
    EDEN_SKIN_UNI(texmat, "u_texmat");             EDEN_SKIN_UNI(color, "u_color");
    EDEN_SKIN_UNI(boneCount, "u_boneCount");       EDEN_SKIN_UNI(lightAmbient, "u_lightAmbient");
    EDEN_SKIN_UNI(lightDiffuse, "u_lightDiffuse"); EDEN_SKIN_UNI(tex, "u_tex");
    EDEN_SKIN_UNI(useTexture, "u_useTexture");     EDEN_SKIN_UNI(fogEnabled, "u_fogEnabled");
    EDEN_SKIN_UNI(fogColor, "u_fogColor");         EDEN_SKIN_UNI(fogStart, "u_fogStart");
    EDEN_SKIN_UNI(fogEnd, "u_fogEnd");
#undef EDEN_SKIN_UNI

    for (int i = 0; i < ATTR_COUNT; ++i) {
        if (!g_stream_vbo[i]) glGenBuffers(1, &g_stream_vbo[i]);
    }
    if (!g_stream_ibo) glGenBuffers(1, &g_stream_ibo);
    std::fprintf(stderr, "[eden-gl] matrix-palette skinning program ready.\n");
    return true;
}

// Is this draw a skinned one? All three conditions are the state Classes/Model.mm's DrawModel
// sets up around its glDrawElements and tears down straight after, so nothing else in the frame
// can accidentally satisfy them.
static bool eden_gl_skinning_active(void) {
    return g_matrix_palette_enabled &&
           g_arrays[ATTR_MATRIXINDEX].enabled && g_arrays[ATTR_MATRIXINDEX].size > 0 &&
           g_arrays[ATTR_WEIGHT].enabled      && g_arrays[ATTR_WEIGHT].size > 0;
}

// The dirty-tracking macros. `force` is the "this program has never been uploaded to" flag; the
// cache write happens unconditionally (before the `force ||`) so a forced upload still leaves the
// shadow correct — evaluating eden_cache_* inside a short-circuit would silently desynchronise it.
#define EDEN_SET1F(cache, loc, val) do { \
        bool _ch = eden_cache_f((cache), (float)(val)); \
        if (force || _ch) { glUniform1f((loc), (cache)); stat_issued(); } else stat_elided(); \
    } while (0)
#define EDEN_SET1I(cache, loc, val) do { \
        bool _ch = eden_cache_i((cache), (GLint)(val)); \
        if (force || _ch) { glUniform1i((loc), (cache)); stat_issued(); } else stat_elided(); \
    } while (0)
#define EDEN_SETFV(cache, loc, val, n) do { \
        bool _ch = eden_cache_fv((cache), (val), (n)); \
        if (force || _ch) { \
            if ((n) == 3) glUniform3fv((loc), 1, (cache)); else glUniform4fv((loc), 1, (cache)); \
            stat_issued(); \
        } else stat_elided(); \
    } while (0)
#define EDEN_SETM4(cache, loc, val) do { \
        bool _ch = eden_cache_m((cache), (val)); \
        if (force || _ch) { glUniformMatrix4fv((loc), 1, GL_FALSE, (cache).m); stat_issued(); } \
        else stat_elided(); \
    } while (0)

static void eden_gl_apply_skin_uniforms(void) {
    eden_gl_use_program(g_skin_program);
    const bool force = !g_skinCache.valid;
    g_skinCache.valid = true;

    EDEN_SETM4(g_skinCache.proj, g_skin_uni.proj, g_projection.stack[g_projection.top]);
    // The palette is one 16-matrix upload, so it is cached as one unit (a 1 KB memcmp against a
    // 1 KB upload that also crosses into JS — the compare wins). Creature draws change bones every
    // draw, so this one usually DOES fire; the point is that the twelve constants below do not.
    {
        bool changed = false;
        for (int i = 0; i < kMaxPaletteMatrices; ++i)
            if (eden_cache_m(g_skinCache.palette[i], g_palette[i])) changed = true;
        if (force || changed) {
            glUniformMatrix4fv(g_skin_uni.palette, kMaxPaletteMatrices, GL_FALSE, g_palette[0].m);
            stat_issued();
        } else stat_elided();
    }
    EDEN_SETM4(g_skinCache.texmat, g_skin_uni.texmat, g_texture.stack[g_texture.top]);
    EDEN_SETFV(g_skinCache.color, g_skin_uni.color, g_current_color, 4);
    EDEN_SET1I(g_skinCache.boneCount, g_skin_uni.boneCount, g_arrays[ATTR_WEIGHT].size);

    // ES1's lit vertex colour for an ambient+diffuse-only light with GL_COLOR_MATERIAL on:
    //   C * (lightModelAmbient + light0Ambient) + C * light0Diffuse * max(N.L, 0)
    GLfloat amb[3] = {g_light_model_ambient[0] + g_light0_ambient[0],
                      g_light_model_ambient[1] + g_light0_ambient[1],
                      g_light_model_ambient[2] + g_light0_ambient[2]};
    EDEN_SETFV(g_skinCache.lightAmbient, g_skin_uni.lightAmbient, amb, 3);
    EDEN_SETFV(g_skinCache.lightDiffuse, g_skin_uni.lightDiffuse, g_light0_diffuse, 3);

    // The sampler unit never changes; upload it once per program object rather than per draw.
    if (force) { glUniform1i(g_skin_uni.tex, 0); stat_issued(); } else stat_elided();
    g_dbg_lastDrawTextured = (g_texture2d_enabled && g_arrays[ATTR_TEXCOORD].enabled);
    EDEN_SET1F(g_skinCache.useTexture, g_skin_uni.useTexture,
               g_dbg_lastDrawTextured ? 1.0f : 0.0f);
    EDEN_SET1F(g_skinCache.fogEnabled, g_skin_uni.fogEnabled,
               (g_fog_enabled && g_fog_mode == GL_LINEAR) ? 1.0f : 0.0f);
    EDEN_SETFV(g_skinCache.fogColor, g_skin_uni.fogColor, g_fog_color, 4);
    EDEN_SET1F(g_skinCache.fogStart, g_skin_uni.fogStart, g_fog_start);
    EDEN_SET1F(g_skinCache.fogEnd, g_skin_uni.fogEnd, g_fog_end);
    eden_gl_active_texture0();
}

static GLsizei eden_gl_type_size(GLenum type) {
    switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE:   return 1;
        case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
        case GL_FIXED: case GL_FLOAT:          return 4;
        default:                               return 4;
    }
}

// Binds every enabled array to its attribute slot. `vertexCeiling` is one past the highest
// vertex the draw will touch — needed only for client-memory arrays, which must be copied into
// a real buffer first (WebGL forbids client-side vertex data outright).
//
// Item #4: every call below is issued only when this slot's specification actually differs from
// what GL was last told. The client-memory branch still uploads unconditionally (the CPU-side
// bytes change every frame by definition) but its glVertexAttribPointer is usually identical draw
// to draw, and so is skipped.
static void eden_gl_setup_attributes(GLsizei vertexCeiling) {
    // Invariant this relies on: eden_gl_restore_bindings always leaves GL_ARRAY_BUFFER equal to
    // the engine's own binding mirror, so that is what is really bound on entry.
    g_path_array_buffer = g_bound_array_buffer;

    // Row #16: client-memory arrays that interleave one vertex struct (Hud/Fire/BlockBreak/object
    // batches — position/color/texcoord sharing one pointer) used to run the per-slot branch below
    // independently, uploading nearly the same bytes into three separate streaming VBOs once per
    // attribute. Detect the sharing here (same nonzero stride, pointers within one stride of each
    // other) and upload the shared span ONCE, into the lowest-numbered member's streaming VBO, with
    // every other member pointing at a byte offset into that same buffer. Arrays with stride 0
    // (tightly packed, single-attribute) or VBO-resident arrays are untouched by this and keep
    // their existing branches below.
    struct UploadGroup { const char* base; GLsizei stride; int leaderSlot; bool uploaded; };
    UploadGroup groups[ATTR_COUNT];
    int groupCount = 0;
    int groupIndexOf[ATTR_COUNT];
    for (int i = 0; i < ATTR_COUNT; ++i) groupIndexOf[i] = -1;
    if (vertexCeiling > 0) {
        for (int slot = 0; slot < ATTR_COUNT; ++slot) {
            const ClientArray& a = g_arrays[slot];
            if (!a.enabled || a.size <= 0 || a.buffer || !a.pointer || a.stride <= 0) continue;
            const char* p = (const char*)a.pointer;
            int found = -1;
            for (int g = 0; g < groupCount; ++g) {
                if (groups[g].stride != a.stride) continue;
                ptrdiff_t diff = p - groups[g].base;
                if (diff >= 0 && diff < groups[g].stride) { found = g; break; }
            }
            if (found == -1) {
                groups[groupCount] = { p, a.stride, slot, false };
                found = groupCount++;
            }
            groupIndexOf[slot] = found;
        }
    }

    for (int slot = 0; slot < ATTR_COUNT; ++slot) {
        const ClientArray& a = g_arrays[slot];
        AttrCache& c = g_attrCache[slot];
        if (!a.enabled || a.size <= 0 || (a.buffer == 0 && a.pointer == nullptr)) {
            if (c.enabled) { glDisableVertexAttribArray(slot); c.enabled = false; stat_issued(); }
            else stat_elided();
            continue;
        }
        // Colors are the one normalized array: ES1 GL_UNSIGNED_BYTE colors are 0..255 mapped to
        // 0..1. Texcoords are NOT normalized even as GL_SHORT — they are atlas tile indices the
        // texture matrix scales (docs/rendering.md), so raw integer values are the point.
        GLboolean normalized =
            (slot == ATTR_COLOR && (a.type == GL_UNSIGNED_BYTE || a.type == GL_BYTE))
                ? GL_TRUE : GL_FALSE;
        GLsizei estride = a.stride ? a.stride : (GLsizei)(a.size * eden_gl_type_size(a.type));

        if (!c.enabled) { glEnableVertexAttribArray(slot); c.enabled = true; stat_issued(); }
        else stat_elided();

        // Does GL already hold exactly this specification for this slot? `buffer` is part of it:
        // glVertexAttribPointer latches the buffer bound AT CALL TIME, so the same offset against
        // a different buffer is a different specification.
        auto spec_matches = [&](GLuint buf, const void* ptr) {
            return c.buffer == buf && c.size == a.size && c.type == a.type &&
                   c.normalized == normalized && c.stride == estride && c.pointer == ptr;
        };
        auto spec_store = [&](GLuint buf, const void* ptr) {
            c.buffer = buf; c.size = a.size; c.type = a.type;
            c.normalized = normalized; c.stride = estride; c.pointer = ptr;
        };
        auto bind_array = [&](GLuint buf) {
            if (g_path_array_buffer == buf) { stat_elided(); return; }
            glBindBuffer(GL_ARRAY_BUFFER, buf);
            g_path_array_buffer = buf;
            stat_issued();
        };

        if (a.buffer) {
            // VBO-resident (TerrainChunk/Graphics): the pointer is already a byte offset. Nothing
            // needs uploading, so an unchanged specification means ZERO calls for this slot —
            // which is the whole win on the terrain path, where six draws share one chunk's VBO.
            if (spec_matches(a.buffer, a.pointer)) { stat_elided(); continue; }
            bind_array(a.buffer);
            glVertexAttribPointer(slot, a.size, a.type, normalized, estride, a.pointer);
            stat_issued();
            spec_store(a.buffer, a.pointer);
        } else {
            // Client memory (Hud/Terrain object batches/Fire/BlockBreak): copy the span the draw
            // will read into this slot's streaming VBO. The span stops at the LAST vertex's own
            // components rather than a full trailing stride, so an interleaved array is never
            // read past its end.
            GLsizeiptr bytes = 0;
            if (vertexCeiling > 0) {
                bytes = (GLsizeiptr)(vertexCeiling - 1) * estride +
                        (GLsizeiptr)(a.size * eden_gl_type_size(a.type));
            }
            bind_array(g_stream_vbo[slot]);
            glBufferData(GL_ARRAY_BUFFER, bytes, a.pointer, GL_STREAM_DRAW);
            stat_issued();
            if (spec_matches(g_stream_vbo[slot], (const void*)0)) { stat_elided(); continue; }
            glVertexAttribPointer(slot, a.size, a.type, normalized, estride, (const void*)0);
            stat_issued();
            spec_store(g_stream_vbo[slot], (const void*)0);
        }
    }
}

// docs/rendering.md: "passes assume their predecessors restored state". The draw path binds its
// own buffers, so it must hand the engine's back before returning — a stale GL_ARRAY_BUFFER
// would land on the NEXT chunk's glBufferData, corrupting a buffer nowhere near the guilty call.
// Item #4: the CONTRACT is unchanged — on return, both bindings are the engine's again. What
// changed is that a restore is only issued when the draw path actually moved the binding. The
// terrain path binds the chunk's own VBO before specifying its arrays, so g_bound_array_buffer
// already IS what the attributes bound — those restores were pure overhead.
static void eden_gl_restore_bindings(void) {
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

static void eden_gl_apply_uniforms(GLenum mode) {
    eden_gl_use_program(g_program);
    const bool force = !g_uniCache.valid;
    g_uniCache.valid = true;

    // The MVP multiply itself is CPU work the cache cannot skip (it is what detects the change),
    // but it is ~64 multiply-adds against a JS crossing — worth doing to find out we can skip.
    Mat4 mvp;
    mat4_multiply(mvp, g_projection.stack[g_projection.top], g_modelview.stack[g_modelview.top]);
    EDEN_SETM4(g_uniCache.mvp, g_uni.mvp, mvp);
    EDEN_SETM4(g_uniCache.mv, g_uni.mv, g_modelview.stack[g_modelview.top]);
    EDEN_SETM4(g_uniCache.texmat, g_uni.texmat, g_texture.stack[g_texture.top]);

    EDEN_SETFV(g_uniCache.color, g_uni.color, g_current_color, 4);
    EDEN_SET1F(g_uniCache.useColorArray, g_uni.useColorArray,
               g_arrays[ATTR_COLOR].enabled ? 1.0f : 0.0f);
    EDEN_SET1F(g_uniCache.usePointSizeArray, g_uni.usePointSizeArray,
               g_arrays[ATTR_POINTSIZE].enabled ? 1.0f : 0.0f);
    EDEN_SET1F(g_uniCache.pointSize, g_uni.pointSize,
               g_pointSizeMax > 0.0f ? g_pointSizeMax : 1.0f);
    EDEN_SETFV(g_uniCache.pointAtten, g_uni.pointAtten, g_pointDistanceAttenuation, 3);

    // Sampler unit: constant for the program's life (see the skin path's identical note).
    if (force) { glUniform1i(g_uni.tex, 0); stat_issued(); } else stat_elided();
    // A point sprite's UV comes from GL_COORD_REPLACE_OES (gl_PointCoord below), not the
    // texcoord client array — SpecialEffects::render() deliberately glDisableClientState(
    // GL_TEXTURE_COORD_ARRAY)s before drawing the smoke/fire GL_POINTS, since real ES1 point
    // sprites never needed one. Gating texturing on that array being enabled therefore starved
    // point-sprite draws of their texture (fell back to a flat vertex-colored square) even
    // though GL_TEXTURE_2D and GL_POINT_SPRITE_OES were both correctly on.
    bool texcoordSourced = g_arrays[ATTR_TEXCOORD].enabled ||
                            (mode == GL_POINTS && g_point_sprite_enabled);
    g_dbg_lastDrawTextured = (g_texture2d_enabled && texcoordSourced);
    EDEN_SET1F(g_uniCache.useTexture, g_uni.useTexture,
               g_dbg_lastDrawTextured ? 1.0f : 0.0f);
    EDEN_SET1F(g_uniCache.texEnvDecal, g_uni.texEnvDecal,
               g_tex_env_mode == GL_DECAL ? 1.0f : 0.0f);
    EDEN_SET1F(g_uniCache.pointSprite, g_uni.pointSprite, mode == GL_POINTS ? 1.0f : 0.0f);

    // Only LINEAR fog is emulated — it is the only mode Graphics::setZFAR ever sets
    // (docs/rendering.md). GL_EXP/GL_EXP2 would silently render as no fog, so they are treated
    // as "not enabled" rather than quietly mis-shaded.
    EDEN_SET1F(g_uniCache.fogEnabled, g_uni.fogEnabled,
               (g_fog_enabled && g_fog_mode == GL_LINEAR) ? 1.0f : 0.0f);
    EDEN_SETFV(g_uniCache.fogColor, g_uni.fogColor, g_fog_color, 4);
    EDEN_SET1F(g_uniCache.fogStart, g_uni.fogStart, g_fog_start);
    EDEN_SET1F(g_uniCache.fogEnd, g_uni.fogEnd, g_fog_end);

    // Gated on the FUNCTION, not just the cap: ES1's default alpha func is GL_ALWAYS (ref 0),
    // and the engine's only glAlphaFunc call is commented out (Graphics.mm:299 — CLAUDE.md #6's
    // "commented-out code is intentional archaeology"). Emulating a GL_ALWAYS test as
    // "discard if a <= 0" would start dropping fully-transparent fragments the real ES1 pipeline
    // keeps, which is a visible behavior change nobody asked for.
    bool alphaTestActive = g_alpha_test_enabled &&
                           (g_alpha_func == GL_GREATER || g_alpha_func == GL_GEQUAL);
    EDEN_SET1F(g_uniCache.alphaTestEnabled, g_uni.alphaTestEnabled,
               alphaTestActive ? 1.0f : 0.0f);
    EDEN_SET1F(g_uniCache.alphaRef, g_uni.alphaRef, g_alpha_ref);

    eden_gl_active_texture0();
}

#undef EDEN_SET1F
#undef EDEN_SET1I
#undef EDEN_SETFV
#undef EDEN_SETM4

void eden_gl_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (!eden_gl_have_context()) { eden_gl_warn_once(); return; }
    if (count <= 0) return;
    g_stats.draws++;
    // Skinned glDrawArrays does not occur today (Classes/Model.mm draws indexed only), but the
    // branch is here rather than in glDrawElements alone so the two paths cannot disagree about
    // which program a given GL state means.
    if (eden_gl_skinning_active()) {
        if (!eden_gl_ensure_skin_program()) return;
        eden_gl_apply_skin_uniforms();
        eden_gl_setup_attributes((GLsizei)first + count);
        glDrawArrays(mode, first, count);
        eden_gl_restore_bindings();
        return;
    }
    if (!eden_gl_ensure_program()) return;
    eden_gl_apply_uniforms(mode);
    // Client arrays are uploaded from their BASE, so `first` stays a valid vertex index into
    // the streaming buffer and is passed through unchanged.
    eden_gl_setup_attributes((GLsizei)first + count);
    glDrawArrays(mode, first, count);
    if (eden_gl_dbg_active()) {
        g_fdbg.drawArrays++; g_fdbg.verts += count;
        if (g_dbg_lastDrawTextured) g_fdbg.texturedDraws++;
        GLenum e = glGetError(); if (e) g_fdbg.glErr = e;
    }
    eden_gl_restore_bindings();
}

void eden_gl_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (!eden_gl_have_context()) { eden_gl_warn_once(); return; }
    if (count <= 0) return;
    g_stats.draws++;
    const bool skinned = eden_gl_skinning_active();
    if (skinned) {
        if (!eden_gl_ensure_skin_program()) return;
        eden_gl_apply_skin_uniforms();
    } else {
        if (!eden_gl_ensure_program()) return;
        eden_gl_apply_uniforms(mode);
    }

    const void* indexOffset = indices;
    GLsizei vertexCeiling = 0;

    if (g_bound_element_buffer == 0) {
        // Client-side indices (BlockBreak.mm, Fire.mm). Two things follow: the indices need
        // copying into a real buffer, and the highest index has to be scanned for, because it —
        // not `count` — is what bounds the vertex data the draw will read.
        GLsizeiptr indexBytes = (GLsizeiptr)count * eden_gl_type_size(type);
        unsigned maxIndex = 0;
        if (indices) {
            if (type == GL_UNSIGNED_SHORT) {
                const unsigned short* p = (const unsigned short*)indices;
                for (GLsizei i = 0; i < count; ++i) if (p[i] > maxIndex) maxIndex = p[i];
            } else if (type == GL_UNSIGNED_BYTE) {
                const unsigned char* p = (const unsigned char*)indices;
                for (GLsizei i = 0; i < count; ++i) if (p[i] > maxIndex) maxIndex = p[i];
            } else if (type == GL_UNSIGNED_INT) {
                const unsigned int* p = (const unsigned int*)indices;
                for (GLsizei i = 0; i < count; ++i) if (p[i] > maxIndex) maxIndex = p[i];
            }
        }
        vertexCeiling = (GLsizei)maxIndex + 1;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_stream_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indexBytes, indices, GL_STREAM_DRAW);
        stat_issued(2);
        // Tell eden_gl_restore_bindings there is something to put back. The VBO-index branch below
        // never moves this binding, so it must NOT pay for a restore.
        g_path_ebo_dirty = true;
        indexOffset = (const void*)0;
    } else {
        // VBO-resident indices (TerrainChunk.mm's opaque pass). Every array in this path is
        // VBO-resident too, so nothing needs uploading and the ceiling is unused. If a mixed
        // case ever appears — VBO indices with client arrays — the max index cannot be read back
        // cheaply and this would under-upload; it does not occur in the engine today (grep:
        // TerrainChunk is the only glDrawElements-with-bound-EBO caller), and it would show up
        // as missing geometry rather than as a crash, so it is called out here rather than
        // guessed at.
        vertexCeiling = 0;
    }

    eden_gl_setup_attributes(vertexCeiling);
    glDrawElements(mode, count, type, indexOffset);
    if (eden_gl_dbg_active()) {
        g_fdbg.drawElements++; g_fdbg.indices += count;
        if (g_dbg_lastDrawTextured) g_fdbg.texturedDraws++;
        GLenum e = glGetError(); if (e) g_fdbg.glErr = e;
    }
    eden_gl_restore_bindings();
}

// ---- Audit row 21/I6: headless state-tracking self-test ----------------------------------
// GROUP 2b's real draw-path dirty-caches (g_uniCache/g_skinCache/g_attrCache, the setup-call
// elision eden_gl_stat() reports) only run once a live WebGL context exists — headless-gl was
// rejected (see this file's own header), so nothing here can drive an actual draw call under
// `node eden.js`, and that half of "the GL shim's state tracking" genuinely needs a live browser
// pass, not a fabricated node-side stub of it.
//
// What IS fully exercised by `node eden.js` and was previously untested (per the audit's own
// finding — "nothing covers the GL shim's state tracking... where a regression is both likely
// and silent") is the GUARDED/no-context bookkeeping every headless boot already depends on:
// Graphics::initGraphics() calls real glGenBuffers/glBindBuffer/glBufferData during
// World::World(), with no canvas to receive them (GROUP 2b's whole reason to exist), and the
// engine's own code trusts the fake object names it gets back to be nonzero and distinct
// (TerrainChunk.mm tests its vertexBuffer handles against 0). A regression here (e.g. the fake
// counter starting at 0, or two calls aliasing the same name) would silently corrupt every
// chunk's buffer bookkeeping in exactly the headless configuration this project's own test suite
// runs under — and nothing asserted it.
// M0 (ROADMAP Phase M): resident GL buffer bytes + create/delete churn. JSON, static buffer,
// same shape as MeshTiming_web.mm's exports and deliberately NOT EDEN_DIAGNOSTICS-gated — the
// whole point is to read it in build-rel / build-relthr.
extern "C" EDEN_GL_EXPORT
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

// Reset the peak/churn accumulators (not the live count/bytes) so a probe can measure one
// window — e.g. a single teleport reload burst — rather than a cumulative-since-boot figure.
extern "C" EDEN_GL_EXPORT
void eden_debug_gl_buffer_bytes_reset(void) {
    g_glbuf_peakBytes = g_glbuf_bytes;
    g_glbuf_peakCount = g_glbuf_count;
    g_glbuf_creates = 0;
    g_glbuf_deletes = 0;
}

#ifdef EDEN_DIAGNOSTICS
extern "C" EDEN_GL_EXPORT
int eden_gl_selftest_run(void) {
    if (eden_gl_have_context()) {
        // Called from a live browser tab, not `node eden.js` — the guarded path this test
        // exists to cover is not engaged here, so there is nothing meaningful to assert.
        std::fprintf(stderr, "[eden-gl-selftest] SKIP: a real context is current (not headless)\n");
        return 1;
    }
    if (eden_gl_context_is_lost()) {
        std::fprintf(stderr, "[eden-gl-selftest] FAIL: context reported lost before any was ever created\n");
        return 0;
    }

    GLuint bufs[3] = {0, 0, 0};
    eden_gl_glGenBuffers(3, bufs);
    if (!bufs[0] || !bufs[1] || !bufs[2]) {
        std::fprintf(stderr, "[eden-gl-selftest] FAIL: glGenBuffers handed back a zero name "
                              "(%u, %u, %u) — TerrainChunk.mm treats 0 as \"no buffer yet\"\n",
                      bufs[0], bufs[1], bufs[2]);
        return 0;
    }
    if (bufs[0] == bufs[1] || bufs[1] == bufs[2] || bufs[0] == bufs[2]) {
        std::fprintf(stderr, "[eden-gl-selftest] FAIL: glGenBuffers aliased two names "
                              "(%u, %u, %u) — two chunks would silently share one buffer\n",
                      bufs[0], bufs[1], bufs[2]);
        return 0;
    }

    GLuint texs[2] = {0, 0};
    eden_gl_glGenTextures(2, texs);
    if (!texs[0] || !texs[1] || texs[0] == texs[1]) {
        std::fprintf(stderr, "[eden-gl-selftest] FAIL: glGenTextures handed back zero/aliased "
                              "names (%u, %u)\n", texs[0], texs[1]);
        return 0;
    }

    // Buffers and textures deliberately SHARE one counter in guarded mode (see g_fake_object_name's
    // own comment) — real GL keeps them in separate namespaces, but nothing here ever reaches a
    // real driver, so a buffer name and a texture name colliding is expected, not a bug; only
    // aliasing WITHIN one call's own outputs (checked above) would be.

    eden_gl_glDeleteBuffers(3, bufs);  // must not crash the guarded path
    eden_gl_glDeleteTextures(2, texs);

    if (eden_gl_have_context()) {
        std::fprintf(stderr, "[eden-gl-selftest] FAIL: guard opened mid-test with no context created\n");
        return 0;
    }

    std::fprintf(stderr, "[eden-gl-selftest] PASS: guarded object-name generation is nonzero, "
                          "distinct, and delete does not crash\n");
    return 1;
}
#endif

} // extern "C"
