// lit_oracle_legacy_macos.cpp — the fixed-function half of --lit-oracle (G.5b-4; renderer plan §7).
// macOS still offers a legacy OpenGL 2.1 context whose fixed-function lighting is the ES 1.1 equation
// (per vertex, GL_COLOR_MATERIAL, local lights, non-local viewer). This file is its own object library
// (native/CMakeLists.txt) so it sees the real <OpenGL/gl.h> and none of the shim's names; it creates a
// private CGL context, draws, reads back, and makes the caller's context current again.
#include "lit_oracle.h"

#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#include <cstdio>
#include <dlfcn.h>

// Every GL entry point comes from the OpenGL framework by dlsym, not by name. Until G.5b-7 that was
// forced: the ES 1.1 shim defined C functions called glEnable, glDrawElements, glLightfv... in this same
// binary, and the static linker bound a plain call to those (the first version of this file drew nothing
// and drove the shim's state). The shim is gone; dlsym stays, so this file needs no legacy GL header's
// prototypes and nothing else in the binary can link to the legacy names.
#define EDEN_ORACLE_GL(X) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glClear, (GLbitfield)) X(void, glEnable, (GLenum)) X(void, glDisable, (GLenum)) \
    X(void, glGenTextures, (GLsizei, GLuint*)) X(void, glDeleteTextures, (GLsizei, const GLuint*)) \
    X(void, glBindTexture, (GLenum, GLuint)) X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*)) \
    X(void, glTexEnvi, (GLenum, GLenum, GLint)) X(void, glMatrixMode, (GLenum)) X(void, glLoadIdentity, (void)) \
    X(void, glTranslatef, (GLfloat, GLfloat, GLfloat)) X(void, glScalef, (GLfloat, GLfloat, GLfloat)) \
    X(void, glLoadMatrixf, (const GLfloat*)) X(void, glLightModelfv, (GLenum, const GLfloat*)) \
    X(void, glMaterialfv, (GLenum, GLenum, const GLfloat*)) X(void, glMaterialf, (GLenum, GLenum, GLfloat)) \
    X(void, glLightfv, (GLenum, GLenum, const GLfloat*)) X(void, glShadeModel, (GLenum)) \
    X(void, glEnableClientState, (GLenum)) X(void, glDisableClientState, (GLenum)) \
    X(void, glVertexPointer, (GLint, GLenum, GLsizei, const GLvoid*)) X(void, glNormalPointer, (GLenum, GLsizei, const GLvoid*)) \
    X(void, glTexCoordPointer, (GLint, GLenum, GLsizei, const GLvoid*)) X(void, glColorPointer, (GLint, GLenum, GLsizei, const GLvoid*)) \
    X(void, glColor4fv, (const GLfloat*)) X(void, glDrawElements, (GLenum, GLsizei, GLenum, const GLvoid*)) \
    X(void, glFinish, (void)) X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*)) \
    X(GLenum, glGetError, (void)) X(const GLubyte*, glGetString, (GLenum)) \
    X(void, glGenFramebuffersEXT, (GLsizei, GLuint*)) X(void, glDeleteFramebuffersEXT, (GLsizei, const GLuint*)) \
    X(void, glBindFramebufferEXT, (GLenum, GLuint)) X(void, glGenRenderbuffersEXT, (GLsizei, GLuint*)) \
    X(void, glDeleteRenderbuffersEXT, (GLsizei, const GLuint*)) X(void, glBindRenderbufferEXT, (GLenum, GLuint)) \
    X(void, glRenderbufferStorageEXT, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void, glFramebufferRenderbufferEXT, (GLenum, GLenum, GLenum, GLuint)) X(GLenum, glCheckFramebufferStatusEXT, (GLenum))

namespace {
#define EDEN_ORACLE_PTR(ret, name, args) ret (*o_##name) args = nullptr;
EDEN_ORACLE_GL(EDEN_ORACLE_PTR)
#undef EDEN_ORACLE_PTR
bool loadGL() {
    void* fw = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_LOCAL);
    if (!fw) return false;
    bool ok = true;
#define EDEN_ORACLE_LOAD(ret, name, args) \
    o_##name = (ret (*) args)dlsym(fw, #name); if (!o_##name) { std::fprintf(stderr, "[lit-oracle] no %s\n", #name); ok = false; }
    EDEN_ORACLE_GL(EDEN_ORACLE_LOAD)
#undef EDEN_ORACLE_LOAD
    return ok;
}
}  // namespace
// From here on every gl* below is the framework's (o_*).
#define glViewport o_glViewport
#define glClearColor o_glClearColor
#define glClear o_glClear
#define glEnable o_glEnable
#define glDisable o_glDisable
#define glGenTextures o_glGenTextures
#define glDeleteTextures o_glDeleteTextures
#define glBindTexture o_glBindTexture
#define glTexParameteri o_glTexParameteri
#define glPixelStorei o_glPixelStorei
#define glTexImage2D o_glTexImage2D
#define glTexEnvi o_glTexEnvi
#define glMatrixMode o_glMatrixMode
#define glLoadIdentity o_glLoadIdentity
#define glTranslatef o_glTranslatef
#define glScalef o_glScalef
#define glLoadMatrixf o_glLoadMatrixf
#define glLightModelfv o_glLightModelfv
#define glMaterialfv o_glMaterialfv
#define glMaterialf o_glMaterialf
#define glLightfv o_glLightfv
#define glShadeModel o_glShadeModel
#define glEnableClientState o_glEnableClientState
#define glDisableClientState o_glDisableClientState
#define glVertexPointer o_glVertexPointer
#define glNormalPointer o_glNormalPointer
#define glTexCoordPointer o_glTexCoordPointer
#define glColorPointer o_glColorPointer
#define glColor4fv o_glColor4fv
#define glDrawElements o_glDrawElements
#define glFinish o_glFinish
#define glReadPixels o_glReadPixels
#define glGetError o_glGetError
#define glGetString o_glGetString
#define glGenFramebuffersEXT o_glGenFramebuffersEXT
#define glDeleteFramebuffersEXT o_glDeleteFramebuffersEXT
#define glBindFramebufferEXT o_glBindFramebufferEXT
#define glGenRenderbuffersEXT o_glGenRenderbuffersEXT
#define glDeleteRenderbuffersEXT o_glDeleteRenderbuffersEXT
#define glBindRenderbufferEXT o_glBindRenderbufferEXT
#define glRenderbufferStorageEXT o_glRenderbufferStorageEXT
#define glFramebufferRenderbufferEXT o_glFramebufferRenderbufferEXT
#define glCheckFramebufferStatusEXT o_glCheckFramebufferStatusEXT

extern "C" int eden_lit_oracle_legacy(const EdenLitScene* scenes, int n, int w, int h, unsigned char* out) {
    if (!loadGL()) return 0;
    CGLContextObj prev = CGLGetCurrentContext();
    const CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24, kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
        kCGLPFADepthSize, (CGLPixelFormatAttribute)24, kCGLPFAAccelerated, (CGLPixelFormatAttribute)0};
    CGLPixelFormatObj pf = nullptr;
    GLint npf = 0;
    if (CGLChoosePixelFormat(attrs, &pf, &npf) != kCGLNoError || !pf) {
        std::fprintf(stderr, "[lit-oracle] no legacy pixel format\n");
        return 0;
    }
    CGLContextObj ctx = nullptr;
    CGLCreateContext(pf, nullptr, &ctx);
    CGLDestroyPixelFormat(pf);
    if (!ctx) { std::fprintf(stderr, "[lit-oracle] no legacy context\n"); return 0; }
    CGLSetCurrentContext(ctx);
    std::printf("[lit-oracle] fixed function: OpenGL %s / %s\n", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));

    GLuint fbo = 0, rb[2] = {0, 0};
    glGenFramebuffersEXT(1, &fbo);
    glGenRenderbuffersEXT(2, rb);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb[0]);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, w, h);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb[0]);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb[1]);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, rb[1]);
    int ok = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT;
    if (!ok) std::fprintf(stderr, "[lit-oracle] legacy framebuffer incomplete\n");

    for (int s = 0; ok && s < n; ++s) {
        const EdenLitScene& sc = scenes[s];
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glDisable(GL_FOG);

        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sc.tw, sc.th, 0, GL_RGBA, GL_UNSIGNED_BYTE, sc.tex);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
        glTranslatef(sc.uvOffset[0], sc.uvOffset[1], 0.0f);
        glScalef(sc.uvScale[0], sc.uvScale[1], 1.0f);
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(sc.proj);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();   // the engine loads its light positions under an identity modelview

        glEnable(GL_LIGHTING);
        glEnable(GL_NORMALIZE);
        glEnable(GL_COLOR_MATERIAL);   // GL_AMBIENT_AND_DIFFUSE, as Graphics::initGraphics leaves it
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, sc.modelAmbient);
        glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, sc.matSpec);
        glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, sc.shininess);
        for (int i = 0; i < 2; ++i) {
            const GLenum L = GL_LIGHT0 + i;
            if (!sc.lightOn[i]) { glDisable(L); continue; }
            glEnable(L);
            glLightfv(L, GL_POSITION, sc.lpos[i]);
            glLightfv(L, GL_AMBIENT, sc.lamb[i]);
            glLightfv(L, GL_DIFFUSE, sc.ldiff[i]);
            glLightfv(L, GL_SPECULAR, sc.lspec[i]);
        }
        glShadeModel(sc.flat ? GL_FLAT : GL_SMOOTH);
        glLoadMatrixf(sc.mv);

        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_NORMAL_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(EdenLitVertex), sc.v[0].pos);
        glNormalPointer(GL_FLOAT, sizeof(EdenLitVertex), sc.v[0].normal);
        glTexCoordPointer(2, GL_FLOAT, sizeof(EdenLitVertex), sc.v[0].uv);
        if (sc.vertexColor) {
            glEnableClientState(GL_COLOR_ARRAY);
            glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(EdenLitVertex), sc.v[0].rgba);
        } else {
            glDisableClientState(GL_COLOR_ARRAY);
            glColor4fv(sc.tint);
        }
        glDrawElements(GL_TRIANGLES, sc.ni, GL_UNSIGNED_SHORT, sc.idx);
        glFinish();
        if (GLenum e = glGetError()) std::fprintf(stderr, "[lit-oracle] legacy GL error 0x%x in %s\n", e, sc.name);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out + (size_t)s * w * h * 4);
        glDeleteTextures(1, &tex);
    }

    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glDeleteRenderbuffersEXT(2, rb);
    glDeleteFramebuffersEXT(1, &fbo);
    CGLSetCurrentContext(prev);
    CGLDestroyContext(ctx);
    return ok;
}
