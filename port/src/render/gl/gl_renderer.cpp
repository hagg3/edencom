// gl_renderer.cpp — the GL backend of Classes/Renderer.h (G.5b-0; plan §5). A draw states everything
// it needs (R4): its own program, its own state through the bridge (applyRenderer), the 1x1 white
// texture when untextured, the tint as a constant colour attribute when the format has none, and
// fog by data. Client-memory vertices and indices stream through the Q.5 rings with the [min,max] copy
// and index rebase the ES 1.1 shim had (G.5b-7 deleted it); buffers, programs, attributes and uniforms go
// through gl_state's caches, so consecutive draws never re-issue what GL already holds.
#include "gl_state.h"
#include "gl_stream.h"
#include "gl_textures.h"
#include "gl_programs.h"
#include "../render_internal.h"

#include <cstring>
#include <unordered_map>

extern "C" void eden_gl_debug_tex_label(const char* label);   // gl_textures.cpp
namespace rgl { extern float g_point_px_scale; }               // gl_context_native.cpp (N.4.11)

namespace rgl {

namespace {

GLuint g_white = 0;
struct BufInfo { R::BufKind kind; R::Usage usage; };
std::unordered_map<GLuint, BufInfo> g_bufKind;

GLuint white() {
    if (g_white) return g_white;
    glGenTextures(1, &g_white);
    bindTextureNow(g_white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const unsigned char px[4] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return g_white;
}

GLenum glPrim(R::Prim p) {
    switch (p) {
        case R::TRIANGLES:      return GL_TRIANGLES;
        case R::TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
        case R::TRIANGLE_FAN:   return GL_TRIANGLE_FAN;
        case R::LINES:          return GL_LINES;
        case R::LINE_STRIP:     return GL_LINE_STRIP;
        case R::POINTS:         return GL_POINTS;
    }
    return GL_TRIANGLES;
}

GLenum glAttrType(uint8_t t) {
    switch (t) {
        case R::A_F32: return GL_FLOAT;
        case R::A_S16: return GL_SHORT;
        case R::A_U8N: case R::A_U8: return GL_UNSIGNED_BYTE;
        default: return GL_FLOAT;
    }
}

GLenum glIndexType(R::IndexType t) {
    return t == R::I_U8 ? GL_UNSIGNED_BYTE : t == R::I_U32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
}
GLsizei indexSize(R::IndexType t) { return t == R::I_U8 ? 1 : t == R::I_U32 ? 4 : 2; }

// u_mvp, recomputed only when either stack changed (their versions only ever grow).
const float* mvp() {
    static unsigned pv = 0, mv = 0;
    static R::Mat4 m;
    if (pv != R::proj().version() || mv != R::mv().version()) {
        R::mat4Multiply(m, R::proj().top(), R::mv().top());
        pv = R::proj().version(); mv = R::mv().version();
    }
    return m.m;
}

void fogUniforms(Program& p) {
    const rcore::DrawState& d = rcore::ds();
    if (d.fogOn) {
        const float c[4] = {d.fogColor.r, d.fogColor.g, d.fogColor.b, d.fogColor.a};
        float range = d.fogEnd - d.fogStart;
        if (range < 1e-6f) range = 1e-6f;   // the shim's max(end - start, 1e-6)
        set4f(p, U_FOGCOLOR, c);
        set1f(p, U_FOGEND, d.fogEnd);
        set1f(p, U_FOGRANGE, range);
    } else {
        // f = clamp(1e4 - d, 0, 1) = 1.0 exactly for every eye distance the window reaches; 1e4 and
        // 1 are exact in fp16, so a mediump fragment stage gets the same answer.
        set1f(p, U_FOGEND, 1e4f);
        set1f(p, U_FOGRANGE, 1.0f);
    }
}

// Attributes from the format. [lo, hi) is the vertex range the draw reads (client memory only);
// returns the rebase (lo when the vertices were streamed, else 0).
int setupAttributes(const R::Geometry& g, int lo, int hi, const R::Rgba& tint, float constSize) {
    const R::VertexFormat& f = *g.fmt;
    GLuint buf = g.vbuf;
    GLintptr baseOff = 0;
    int base = 0;
    if (!buf) {
        base = lo;
        const char* src = (const char*)g.vmem + (size_t)lo * f.stride;
        const GLsizeiptr bytes = (GLsizeiptr)(hi - lo) * f.stride;
        if (bytes > 0) {
            bindPathArray(g_vring.name);
            GLintptr off = 0;
            void* dst = streamBegin(g_vring, bytes, (GLsizeiptr)((uintptr_t)src & 3), &off);
            std::memcpy(dst, src, (size_t)bytes);
            streamEnd(g_vring, off, bytes);
            baseOff = off;
        }
        buf = g_vring.name;
    }
    const R::Attr* attrs[ATTR_COUNT] = {&f.pos, &f.color, &f.uv, &f.normal, &f.size, &f.index, &f.weight};
    for (int slot = 0; slot < ATTR_COUNT; ++slot) {
        const R::Attr& a = *attrs[slot];
        if (a.type == R::A_NONE) {
            attribDisable(slot);
            if (slot == ATTR_COLOR) attribConstant(slot, tint.r, tint.g, tint.b, tint.a);
            else if (slot == ATTR_POINTSIZE) attribConstant(slot, constSize, 0.0f, 0.0f, 1.0f);
            continue;
        }
        attribPointer(slot, buf, a.comps, glAttrType(a.type), a.type == R::A_U8N ? GL_TRUE : GL_FALSE,
                      f.stride, (const void*)(baseOff + (GLintptr)a.offset));
    }
    return base;
}

template <typename T>
void minMax(const void* idx, int first, int count, unsigned& lo, unsigned& hi) {
    const T* p = (const T*)idx + first;
    lo = 0xFFFFFFFFu; hi = 0;
    for (int i = 0; i < count; ++i) { if (p[i] < lo) lo = p[i]; if (p[i] > hi) hi = p[i]; }
}
template <typename T>
void rebase(void* dst, const void* idx, int first, int count, unsigned base) {
    const T* p = (const T*)idx + first;
    T* q = (T*)dst;
    for (int i = 0; i < count; ++i) q[i] = (T)(p[i] - base);
}

// The shared tail of every draw: attributes, indices, the draw, the engine's bindings handed back.
void submit(const R::Geometry& g, const R::Rgba& tint, float constSize) {
    if (!g.fmt || g.count <= 0) return;
    if (!g.vbuf && !g.vmem) return;
    if (g.itype != R::I_NONE && g.ibuf && !g.vbuf) {
        if (rcore::inv().contractFails++ == 0) rcore::fail("a draw with buffer indices over client-memory vertices");
        return;
    }
    stats().draws++;
    stats().verts += g.count;
    const GLenum prim = glPrim(g.prim);
    if (g.itype == R::I_NONE) {
        const int base = setupAttributes(g, g.first, g.first + g.count, tint, constSize);
        glDrawArrays(prim, g.first - base, g.count);
    } else if (g.ibuf) {
        setupAttributes(g, 0, 0, tint, constSize);
        bindPathElements(g.ibuf);
        glDrawElements(prim, g.count, glIndexType(g.itype), (const void*)(uintptr_t)(g.first * indexSize(g.itype)));
    } else {
        if (!g.imem) return;
        unsigned lo = 0, hi = 0;
        if (g.itype == R::I_U8) minMax<unsigned char>(g.imem, g.first, g.count, lo, hi);
        else if (g.itype == R::I_U16) minMax<unsigned short>(g.imem, g.first, g.count, lo, hi);
        else minMax<unsigned int>(g.imem, g.first, g.count, lo, hi);
        const unsigned base = (unsigned)setupAttributes(g, g.vbuf ? 0 : (int)lo, g.vbuf ? 0 : (int)hi + 1, tint, constSize);
        bindPathElements(g_iring.name);
        const GLsizeiptr bytes = (GLsizeiptr)g.count * indexSize(g.itype);
        GLintptr off = 0;
        void* dst = streamBegin(g_iring, bytes, 0, &off);
        if (g.itype == R::I_U8) rebase<unsigned char>(dst, g.imem, g.first, g.count, base);
        else if (g.itype == R::I_U16) rebase<unsigned short>(dst, g.imem, g.first, g.count, base);
        else rebase<unsigned int>(dst, g.imem, g.first, g.count, base);
        streamEnd(g_iring, off, bytes);
        glDrawElements(prim, g.count, glIndexType(g.itype), (const void*)off);
    }
    restoreBindings();
}

Program* begin(ProgId id, R::Tex t) {
    Program* p = program(id);
    if (!p) return nullptr;
    ensureRings();
    applyRenderer(rcore::ds().state, t ? (GLuint)t : white());
    useProgram(p->id);
    set1i(*p, U_TEX, 0);
    activeTexture0();
    return p;
}

void uvUniform(Program& p) {
    float so[4];
    R::uvScaleOffset(so);
    set4f(p, U_UVSO, so);
}

}  // namespace

// ---- frame ---------------------------------------------------------------------------------------
void beginFrame() {}
void endFrame() {}

// ---- draws ---------------------------------------------------------------------------------------
void drawWorld(const R::Geometry& g, R::Tex t, const R::Rgba& tint, float alphaRef) {
    Program* p = begin(alphaVariant(P_WORLD, alphaRef), t);
    if (!p) return;
    setMat4(*p, U_MVP, mvp());
    setMat4(*p, U_MV, R::mv().top().m);
    uvUniform(*p);
    fogUniforms(*p);
    set1f(*p, U_ALPHAREF, alphaRef);
    submit(g, tint, 1.0f);
}

void draw2D(const R::Geometry& g, R::Tex t, const R::Rgba& tint) {
    Program* p = begin(P_UI, t);
    if (!p) return;
    setMat4(*p, U_MVP, mvp());
    submit(g, tint, 1.0f);
}

void drawSprites(const R::Geometry& g, R::Tex t, const R::SpriteParams& sp) {
    Program* p = begin(P_SPRITE, t);
    if (!p) return;
    setMat4(*p, U_MVP, mvp());
    setMat4(*p, U_MV, R::mv().top().m);
    // N.4.11: in a scaled scene pass dividing the coefficients by s^2 multiplies the size by s.
    const float s = g_point_px_scale;
    const float k = 1.0f / (s * s);
    const float atten[3] = {sp.atten[0] * k, sp.atten[1] * k, sp.atten[2] * k};
    set3f(*p, U_POINTATTEN, atten);
    fogUniforms(*p);
    const R::Rgba white = {1, 1, 1, 1};
    submit(g, white, sp.size);
}

void drawSkinned(const R::Geometry& g, R::Tex t, const R::Rgba& tint, const float* palette, int nmat,
                 int bonesPerVertex) {
    Program* p = begin(P_SKINNED, t);
    if (!p) return;
    setMat4(*p, U_PROJ, R::proj().top().m);
    setPalette(*p, palette, nmat);
    uvUniform(*p);
    set1i(*p, U_BONECOUNT, bonesPerVertex);
    const R::LightSet& L = rcore::ds().lights;
    const float amb[3] = {L.modelAmbient[0] + L.light[0].ambient[0], L.modelAmbient[1] + L.light[0].ambient[1],
                          L.modelAmbient[2] + L.light[0].ambient[2]};
    set3f(*p, U_LIGHTAMB, amb);
    set3f(*p, U_LIGHTDIFF, L.light[0].diffuse);
    fogUniforms(*p);
    submit(g, tint, 1.0f);
}

void drawLit(const R::Geometry& g, R::Tex t, const R::Rgba& tint) {
    Program* p = begin(alphaVariant(rcore::ds().lights.flat ? P_LIT_FLAT : P_LIT, -1.0f), t);
    if (!p) return;
    setMat4(*p, U_MVP, mvp());
    setMat4(*p, U_MV, R::mv().top().m);
    uvUniform(*p);
    fogUniforms(*p);
    set1f(*p, U_ALPHAREF, -1.0f);
    const R::LightSet& L = rcore::ds().lights;
    float amb[3] = {L.modelAmbient[0], L.modelAmbient[1], L.modelAmbient[2]};
    static const float kOffPos[4] = {0, 0, 1, 0}, kZero[3] = {0, 0, 0};
    const Uni posU[2] = {U_LPOS0, U_LPOS1}, diffU[2] = {U_LDIFF0, U_LDIFF1}, specU[2] = {U_LSPEC0, U_LSPEC1};
    for (int i = 0; i < 2; ++i) {
        const R::Light& l = L.light[i];
        if (!l.on) { set4f(*p, posU[i], kOffPos); set3f(*p, diffU[i], kZero); set3f(*p, specU[i], kZero); continue; }
        for (int c = 0; c < 3; ++c) amb[c] += l.ambient[c];
        const float spec[3] = {l.specular[0] * L.matSpecular[0], l.specular[1] * L.matSpecular[1],
                               l.specular[2] * L.matSpecular[2]};
        set4f(*p, posU[i], l.pos);
        set3f(*p, diffU[i], l.diffuse);
        set3f(*p, specU[i], spec);
    }
    set3f(*p, U_AMBSUM, amb);
    set1f(*p, U_SHININESS, L.shininess);
    submit(g, tint, 1.0f);
}

bool readPixels(int x, int y, int w, int h, void* rgba) {
    if (!rgba || w <= 0 || h <= 0) return false;
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return true;
}

// The clear (R::clear): glClearColor every time (uncached: cheap, and nothing can go stale), and a
// depth clear writes depth whatever the last draw's mask was.
void clear(bool color, bool depth, const R::Rgba& c) {
    GLbitfield mask = 0;
    if (color) { glClearColor(c.r, c.g, c.b, c.a); mask |= GL_COLOR_BUFFER_BIT; }
    if (depth) { depthWriteNow(true); mask |= GL_DEPTH_BUFFER_BIT; }
    if (mask) glClear(mask);
}

unsigned takeError() { return (unsigned)glGetError(); }

const char* driverString(int which) {
    const GLenum name = which == R::DS_RENDERER ? GL_RENDERER : which == R::DS_VERSION ? GL_VERSION : GL_EXTENSIONS;
    return (const char*)glGetString(name);
}

// ---- textures --------------------------------------------------------------------------------------
namespace {
void texFormat(R::TexFormat f, GLenum* format, GLenum* type) {
    *type = GL_UNSIGNED_BYTE;
    switch (f) {
        case R::TF_RGBA8:    *format = GL_RGBA; break;
        case R::TF_RGB8:     *format = GL_RGB; break;
        case R::TF_RGB565:   *format = GL_RGB;  *type = GL_UNSIGNED_SHORT_5_6_5; break;
        case R::TF_RGBA4444: *format = GL_RGBA; *type = GL_UNSIGNED_SHORT_4_4_4_4; break;
        case R::TF_RGBA5551: *format = GL_RGBA; *type = GL_UNSIGNED_SHORT_5_5_5_1; break;
        case R::TF_L8:       *format = GL_LUMINANCE; break;
        case R::TF_A8:       *format = GL_ALPHA; break;
        case R::TF_LA88:     *format = GL_LUMINANCE_ALPHA; break;
    }
}
// Rows are tightly packed (an RGB8 or L8 row need not be 4-byte aligned); the engine's own alignment
// is restored after.
struct UnpackAlign1 {
    GLint prev = 4;
    UnpackAlign1() { glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev); if (prev != 1) glPixelStorei(GL_UNPACK_ALIGNMENT, 1); }
    ~UnpackAlign1() { if (prev != 1) glPixelStorei(GL_UNPACK_ALIGNMENT, prev); }
};
}  // namespace

R::Tex texCreate(const R::TexDesc& d, const void* px) {
    GLuint t = 0;
    glGenTextures(1, &t);
    if (!t) return 0;
    bindTextureNow(t);
    const GLint mag = d.linear ? GL_LINEAR : GL_NEAREST;
    const GLint min = d.mipmaps ? (d.linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR) : mag;
    texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min);
    texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
    texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, d.clampS ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, d.clampT ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    if (d.mipmaps) texParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, 1);   // ES 1.1's, deferred (gl_textures)
    GLenum format, type;
    texFormat(d.format, &format, &type);
    if (d.label) eden_gl_debug_tex_label(d.label);
    {
        UnpackAlign1 a;
        texImage2D(GL_TEXTURE_2D, 0, (GLint)format, d.w, d.h, 0, format, type, px);
    }
    if (d.label) eden_gl_debug_tex_label(nullptr);
    return t;
}

void texUpdate(R::Tex t, int x, int y, int w, int h, R::TexFormat f, const void* px) {
    if (!t || !px || w <= 0 || h <= 0) return;
    GLenum format, type;
    texFormat(f, &format, &type);
#if !defined(EDEN_GL_ES_DIALECT)
    // texImage2D stored L/A/LA as R8/RG8 (Q.4); a sub-image must name the stored layout.
    if (format == GL_LUMINANCE || format == GL_ALPHA) format = 0x1903 /* GL_RED */;
    else if (format == GL_LUMINANCE_ALPHA) format = 0x8227 /* GL_RG */;
#endif
    bindTextureNow((GLuint)t);
    UnpackAlign1 a;
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, format, type, px);
}

void texSampling(R::Tex t, const R::TexDesc& d) {
    if (!t) return;
    bindTextureNow((GLuint)t);
    const GLint mag = d.linear ? GL_LINEAR : GL_NEAREST;
    const GLint min = d.mipmaps ? (d.linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR) : mag;
    texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min);
    texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
    texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, d.clampS ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, d.clampT ? GL_CLAMP_TO_EDGE : GL_REPEAT);
}

void texDestroy(R::Tex t) {
    if (!t) return;
    GLuint name = (GLuint)t;
    forgetTexture(name);
    glDeleteTextures(1, &name);
}

// ---- buffers ---------------------------------------------------------------------------------------
R::Buf bufCreate(R::BufKind k, R::Usage u) {
    GLuint b = 0;
    glGenBuffers(1, &b);
    noteBufferCreates(1);
    if (b) g_bufKind[b] = BufInfo{k, u};
    return b;
}

static void bufBind(R::Buf b) {
    if (g_bufKind[(GLuint)b].kind == R::BK_INDEX) bindPathElements((GLuint)b);
    else bindPathArray((GLuint)b);
}
static GLenum bufTarget(R::Buf b) {
    return g_bufKind[(GLuint)b].kind == R::BK_INDEX ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
}

void bufData(R::Buf b, const void* data, size_t bytes) {
    if (!b) return;
    bufBind(b);
    const R::Usage u = g_bufKind[(GLuint)b].usage;
    glBufferData(bufTarget(b), (GLsizeiptr)bytes, data,
                 u == R::U_STREAM ? GL_STREAM_DRAW : u == R::U_DYNAMIC ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    noteBufferDataFor((GLuint)b, bytes);
    restoreBindings();
}

void bufSubData(R::Buf b, size_t offset, const void* data, size_t bytes) {
    if (!b || !data) return;
    bufBind(b);
    glBufferSubData(bufTarget(b), (GLintptr)offset, (GLsizeiptr)bytes, data);
    restoreBindings();
}

void bufDestroy(R::Buf b) {
    if (!b) return;
    GLuint name = (GLuint)b;
    forgetDeletedBuffers(1, &name);
    glDeleteBuffers(1, &name);
    g_bufKind.erase(name);
}

void invalidateAll() {
    invalidate();
    invalidateRings();
    invalidatePrograms();
    g_white = 0;
    g_bufKind.clear();
}

}  // namespace rgl
