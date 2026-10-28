// gl_programs.h — shader compilation (shared with the shim's two programs) and the renderer's five
// programs (plan §6, R5/R6): 1 world, 2 ui, 3 sprite, 4 skinned, 5 lit. Branchless: no uniform flags.
// Untextured binds a 1x1 white texture; a format without colour gets the tint as a constant attribute;
// fog off is fog whose end is far past any eye distance; alphaRef -1 never discards.
#ifndef EDEN_RENDER_GL_PROGRAMS_H
#define EDEN_RENDER_GL_PROGRAMS_H

#include "gl_state.h"

namespace rgl {

// The dialect preamble (EDEN_GL_ES_DIALECT: is the CONTEXT ES?) and one shader stage. `es300` selects
// GLSL ES 3.00 / 330 core; otherwise the shim's aliased ES 1.00 main pair (EDEN_ATTR/EDEN_VARY/...).
const char* shaderPreamble(GLenum stage, bool es300);
GLuint compileShader(GLenum stage, const char* src, bool es300, const char* defs = nullptr);
// Link with every attribute location bound to its AttrSlot. 0 on failure (logged, and appended to *log).
GLuint linkProgram(GLuint vs, GLuint fs, const char* name, std::string* log = nullptr);

// P_LIT_FLAT is program 5 compiled with a `flat` colour (ES 1.1 GL_FLAT: the triangle's last vertex
// colours it), for the lit draws 2.1.1 made under glShadeModel(GL_FLAT) (block-break debris; G.5b-4).
enum ProgId { P_WORLD = 0, P_UI, P_SPRITE, P_SKINNED, P_LIT, P_LIT_FLAT, P_PROG_COUNT };

enum Uni {
    U_MVP = 0, U_MV, U_PROJ, U_UVSO, U_TEX,
    U_FOGCOLOR, U_FOGEND, U_FOGRANGE, U_ALPHAREF,
    U_POINTATTEN,
    U_PALETTE, U_BONECOUNT, U_LIGHTAMB, U_LIGHTDIFF,
    U_AMBSUM, U_LPOS0, U_LPOS1, U_LDIFF0, U_LDIFF1, U_LSPEC0, U_LSPEC1, U_SHININESS,
    U_COUNT
};

struct Program {
    GLuint id = 0;
    bool   tried = false;          // a failed link is not retried every draw
    GLint  loc[U_COUNT];
    float  cache[U_COUNT][16];     // the last value sent (U_PALETTE: the first matrix; see paletteCache)
    bool   valid[U_COUNT];
    float  paletteCache[16 * 16];
    int    paletteN = 0;
};

// The program, compiled on first use (a context is live). nullptr if it failed to link.
Program* program(ProgId p);
void set1f(Program& p, Uni u, float v);
void set1i(Program& p, Uni u, int v);
void set3f(Program& p, Uni u, const float* v);
void set4f(Program& p, Uni u, const float* v);
void setMat4(Program& p, Uni u, const float* m);
void setPalette(Program& p, const float* m, int n);
void invalidatePrograms();

}  // namespace rgl

#endif  // EDEN_RENDER_GL_PROGRAMS_H
