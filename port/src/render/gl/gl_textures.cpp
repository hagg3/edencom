// gl_textures.cpp — see gl_textures.h. Moved from the ES 1.1 shim's glTexParameteri / glTexImage2D
// wrappers (G.5b-0); the code and its history notes are unchanged.
#include "gl_textures.h"

#include <cstdio>

namespace {
bool g_pending_generate_mipmap = false;
// N.4.9: cumulative texture upload bytes (level-0 size, x4/3 when a mip chain is generated). Not a
// live figure, since deletes are not matched to sizes; the engine almost never deletes a texture,
// so for "what did start-up upload" it is the number. Read by eden_debug_gl_tex_bytes() below.
unsigned long long g_tex_upload_bytes = 0;
unsigned g_tex_uploads = 0;

// N.4.9: a glFlush after EVERY texture upload, on by default (2026-10-04). Without it the iOS GL
// driver on an iPad Air 2 (A8X) held 192 MB of IOAccelerator memory for 45 MB of textures after the
// ~150 start-up uploads, and +80 MB more on the first frames, while the same uploads each followed
// by a flush cost 1.0-1.3x nominal: the driver keeps per-upload staging and glGenerateMipmap's
// working memory until the work is submitted, and nothing submitted it until the first frame
// presented. Measured: 64z footprint 338 -> 145 MB (criterion 3 is 300). The Mac and web already
// cost ~1.0x and are unchanged by it. `--tex-exp=noflush` turns it off for an A/B.
// Write-up: WORKING/n49-r4-device-results-2026-10-04.md.
bool g_tex_flush = true;

// N.4.9 per-texture probe and texture experiments (diagnostics; all off by default).
//  - g_tex_probe: set by native --mem-trace=2 (HeapProbe_native.cpp). Called around each upload
//    with phase 0 = before glTexImage2D, 1 = after it, 2 = after glGenerateMipmap (and the flush
//    above), 3 = after a glFinish the probe itself issues.
//  - g_tex_exp (native --tex-exp=): EDEN_TEX_EXP_NOMIPS never builds a mip chain (any *_MIPMAP_*
//    min filter becomes GL_LINEAR, else the texture is incomplete and samples black);
//    EDEN_TEX_EXP_STORAGE allocates with glTexStorage2D (immutable, exact level count) and fills
//    level 0 with glTexSubImage2D — ES 3.0 only, so iOS only. On the iPad: nomips 64z 119 MB
//    (with the flush off); storage made it WORSE (402 MB against 338).
//    EDEN_TEX_EXP_FP16UV (I.1) is read by gl_programs.cpp, not here: it rounds every fragment
//    stage's texcoord to fp16, the pre-I.1 iOS mediump varying, so the Mac shows that look.
//  - g_tex_label: the asset name Texture2D.cpp is uploading, for the probe's line only.
enum { EDEN_TEX_EXP_NOMIPS = 1, EDEN_TEX_EXP_STORAGE = 2 };
void (*g_tex_probe)(int phase, const char* label, int w, int h, unsigned format, unsigned type,
                    int mips) = nullptr;
int g_tex_exp = 0;
const char* g_tex_label = nullptr;
}
extern "C" void eden_gl_set_tex_probe(void (*fn)(int, const char*, int, int, unsigned, unsigned, int)) {
    g_tex_probe = fn;
}
extern "C" void eden_gl_set_tex_experiments(int bits) { g_tex_exp = bits; }
extern "C" void eden_gl_set_tex_flush(int on) { g_tex_flush = (on != 0); }
extern "C" int eden_gl_tex_experiments(void) { return g_tex_exp; }
extern "C" void eden_gl_debug_tex_label(const char* label) { g_tex_label = label; }

namespace rgl {

void texParameteri(GLenum target, GLenum pname, GLint param) {
    if (pname == GL_GENERATE_MIPMAP) {
        g_pending_generate_mipmap = (param != 0) && !(g_tex_exp & EDEN_TEX_EXP_NOMIPS);
        return; // no GLES2 equivalent enum to forward — see comment above
    }
    if ((g_tex_exp & EDEN_TEX_EXP_NOMIPS) && pname == GL_TEXTURE_MIN_FILTER &&
        param != GL_NEAREST && param != GL_LINEAR) param = GL_LINEAR;
    glTexParameteri(target, pname, param);
}

#if defined(EDEN_PLATFORM_IOS)
// EDEN_TEX_EXP_STORAGE: the sized internal format for an unsized ES upload, or 0 when ES 3.0 has
// none (LUMINANCE / ALPHA / LUMINANCE_ALPHA), in which case the upload takes the ordinary path.
static GLenum eden_tex_sized_format(GLenum format, GLenum type) {
    if (format == GL_RGBA && type == GL_UNSIGNED_BYTE) return GL_RGBA8;
    if (format == GL_RGBA && type == GL_UNSIGNED_SHORT_4_4_4_4) return GL_RGBA4;
    if (format == GL_RGBA && type == GL_UNSIGNED_SHORT_5_5_5_1) return GL_RGB5_A1;
    if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) return GL_RGB565;
    if (format == GL_RGB && type == GL_UNSIGNED_BYTE) return GL_RGB8;
    return 0;
}
#endif
#if !defined(EDEN_GL_ES_DIALECT)
// Q.4 (F3): desktop core GL removed LUMINANCE / ALPHA / LUMINANCE_ALPHA (GL_INVALID_ENUM, no
// texture). Each becomes a sized R8/RG8 texture plus a per-texture swizzle (GL 3.3 core; the
// context is 4.1) that samples exactly what the ES 1 format did: L -> (L,L,L,1), A -> (0,0,0,A),
// LA -> (L,L,L,A). ES 3.0 (iOS) still takes the unsized formats, so it never comes here.
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_RG
#define GL_RG 0x8227
#endif
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RG8
#define GL_RG8 0x822B
#endif
#ifndef GL_TEXTURE_SWIZZLE_R
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#define GL_TEXTURE_SWIZZLE_G 0x8E43
#define GL_TEXTURE_SWIZZLE_B 0x8E44
#define GL_TEXTURE_SWIZZLE_A 0x8E45
#endif
static bool eden_tex_core_legacy_format(GLenum target, GLenum* format, GLint* internalformat) {
    GLint sw[4];
    if (*format == GL_LUMINANCE_ALPHA) {
        *format = GL_RG; *internalformat = GL_RG8;
        sw[0] = sw[1] = sw[2] = GL_RED; sw[3] = GL_GREEN;
    } else if (*format == GL_LUMINANCE) {
        *format = GL_RED; *internalformat = GL_R8;
        sw[0] = sw[1] = sw[2] = GL_RED; sw[3] = GL_ONE;
    } else if (*format == GL_ALPHA) {
        *format = GL_RED; *internalformat = GL_R8;
        sw[0] = sw[1] = sw[2] = GL_ZERO; sw[3] = GL_RED;
    } else {
        return false;
    }
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, sw[0]);
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, sw[1]);
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, sw[2]);
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, sw[3]);
    return true;
}
#endif
void texImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels) {
    {
        const int mips = (g_pending_generate_mipmap && level == 0) ? 1 : 0;
        if (g_tex_probe) g_tex_probe(0, g_tex_label, width, height, format, type, mips);
        const GLenum esFormat = format;   // the byte accounting below counts the ES 1 layout
#if !defined(EDEN_GL_ES_DIALECT)
        // The swizzle is texture-object state, so an RGB(A) re-upload into a name that held an
        // L/LA/A image would keep it. Nothing does that today (Texture2D gens a fresh name per
        // image); a caller that starts to must reset the swizzle.
        eden_tex_core_legacy_format(target, &format, &internalformat);
#endif
#if defined(EDEN_PLATFORM_IOS)
        GLenum sized = 0;
        if ((g_tex_exp & EDEN_TEX_EXP_STORAGE) && level == 0 && border == 0)
            sized = eden_tex_sized_format(format, type);
        if (sized) {
            GLsizei levels = 1;
            if (mips) for (GLsizei m = width > height ? width : height; m > 1; m >>= 1) ++levels;
            glTexStorage2D(target, levels, sized, width, height);
            if (pixels) glTexSubImage2D(target, 0, 0, 0, width, height, format, type, pixels);
        } else
#endif
        glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
        if (g_tex_probe) g_tex_probe(1, g_tex_label, width, height, esFormat, type, mips);
        {
            unsigned long long bpp = 4;
            if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 ||
                type == GL_UNSIGNED_SHORT_5_5_5_1) bpp = 2;
            else if (esFormat == GL_ALPHA || esFormat == GL_LUMINANCE) bpp = 1;
            else if (esFormat == GL_LUMINANCE_ALPHA) bpp = 2;
            unsigned long long b = (unsigned long long)width * (unsigned long long)height * bpp;
            if (g_pending_generate_mipmap && level == 0) b = b * 4 / 3;
            g_tex_upload_bytes += b;
            g_tex_uploads++;
        }
        if (g_pending_generate_mipmap && level == 0) glGenerateMipmap(target);
        g_pending_generate_mipmap = false;
        if (g_tex_flush) glFlush();   // N.4.9: see g_tex_flush
        if (g_tex_probe) {
            g_tex_probe(2, g_tex_label, width, height, esFormat, type, mips);
            glFinish();
            g_tex_probe(3, g_tex_label, width, height, esFormat, type, mips);
        }
    }
}

}  // namespace rgl

// The R:: side (R::texCreate / texUpdate / texDestroy) is in gl_renderer.cpp.

// N.4.9: what the texture uploads add up to (see g_tex_upload_bytes).
extern "C" const char* eden_debug_gl_tex_bytes(void) {
    static char buf[96];
    std::snprintf(buf, sizeof(buf), "{\"uploads\":%u,\"bytes\":%llu}", g_tex_uploads, g_tex_upload_bytes);
    return buf;
}
