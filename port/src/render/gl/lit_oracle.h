// lit_oracle.h — --lit-oracle's scene description (G.5b-4; renderer plan §7). Plain C: it crosses into
// lit_oracle_legacy_macos.cpp, an object library of its own that sees the real <OpenGL/gl.h> (fixed
// function, a CGL legacy 2.1 context) and nothing of the shim. gl_selftest.cpp draws the same scenes
// through R::drawLit on the engine's core context and compares the two readbacks.
#ifndef EDEN_RENDER_GL_LIT_ORACLE_H
#define EDEN_RENDER_GL_LIT_ORACLE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { float pos[3], normal[3]; unsigned char rgba[4]; float uv[2]; } EdenLitVertex;

typedef struct {
    const char* name;
    const EdenLitVertex* v; int nv;
    const unsigned short* idx; int ni;          // GL_TRIANGLES
    float proj[16], mv[16];                     // column-major
    float uvScale[2], uvOffset[2];              // the texture matrix (scale, then offset)
    int   vertexColor;                          // 1: the vertices' rgba; 0: `tint` (glColor)
    float tint[4];
    int   flat;                                 // glShadeModel(GL_FLAT)
    int   lightOn[2];
    float lpos[2][4], lamb[2][4], ldiff[2][4], lspec[2][4];   // eye space (loaded under identity)
    float modelAmbient[4], matSpec[4], shininess;
    const unsigned char* tex; int tw, th;       // RGBA8, nearest, repeat; GL_MODULATE
} EdenLitScene;

// Renders each scene into a w x h RGBA8 framebuffer through real ES 1.1-equivalent fixed-function GL
// (clear colour 0.1, 0.2, 0.3) and writes w*h*4 bytes per scene to out (bottom-up). The caller's GL
// context is current again on return. 0 when no legacy context exists (not macOS, or none offered).
int eden_lit_oracle_legacy(const EdenLitScene* scenes, int n, int w, int h, unsigned char* out);

#ifdef __cplusplus
}
#endif
#endif  // EDEN_RENDER_GL_LIT_ORACLE_H
