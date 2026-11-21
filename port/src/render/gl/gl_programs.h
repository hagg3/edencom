// gl_programs.h — shader compilation (shared with the shim's two programs) and the renderer's five
// programs (plan §6, R5/R6): 1 world, 2 ui, 3 sprite, 4 skinned, 5 lit. Branchless: no uniform flags.
// Untextured binds a 1x1 white texture; a format without colour gets the tint as a constant attribute;
// fog off is fog whose end is far past any eye distance; alphaRef -1 never discards (and picks a program without the test, PW.3).
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
// SH.1: program 1 (world) is one program per feature key, built lazily; the other programs keep fixed ids.
// The bits are in the order their #defines are emitted, so a key's source string is the one the hand-listed
// twins used (NO_DISCARD = PW.3 no `discard`, HEIGHT_FOG = AT.5, CLOUD_SHADOW = AT.6, WATER_TINT = AT.8,
// WATER_GLINT = AT.11, SUN_SHADOW = SH.2). WATER_TINT needs NO_DISCARD and WATER_GLINT needs WATER_TINT (worldKeyValid).
enum WorldFeat { WF_NO_DISCARD = 1, WF_HEIGHT_FOG = 2, WF_CLOUD_SHADOW = 4, WF_WATER_TINT = 8, WF_WATER_GLINT = 16,
                 WF_SUN_SHADOW = 32, WF_KEYS = 64 };
enum ProgId { P_UI = 0, P_SPRITE, P_SKINNED, P_LIT, P_LIT_FLAT,
              P_LIT_OPAQUE, P_LIT_FLAT_OPAQUE,   // PW.3: no `discard` (alphaRef < 0 draws)
              P_PRECIP,   // AT.2b: rain streaks (R::drawPrecip), built on first use: never while precipitation is off
              P_CLOUDS,   // AT.6: the cloud layer (R::drawClouds), built on first use
              P_DEPTH, P_DEPTH_ALPHA,   // SH.2: the sun-shadow depth pass (R::drawDepth), built on first use
              P_WORLD_BASE,   // P_WORLD_BASE + key: the world programs (worldProg)
              P_PROG_COUNT = P_WORLD_BASE + WF_KEYS };
inline ProgId worldProg(unsigned key) { return (ProgId)(P_WORLD_BASE + key); }
bool worldKeyValid(unsigned key);
int  programCount();   // programs reachable (every valid world key + the others): what compileAll links


enum Uni {
    U_MVP = 0, U_MV, U_PROJ, U_UVSO, U_TEX,
    U_FOGCOLOR, U_FOGEND, U_FOGRANGE, U_ALPHAREF,
    U_POINTATTEN,
    U_PALETTE, U_BONECOUNT, U_LIGHTAMB, U_LIGHTDIFF,
    U_AMBSUM, U_LPOS0, U_LPOS1, U_LDIFF0, U_LDIFF1, U_LSPEC0, U_LSPEC1, U_SHININESS,
    U_WORLDLIGHT,   // DN.5: program 1 only
    U_PBOX0, U_PBOX1, U_PMOVE, U_PSLANT, U_PCOLOR, U_PTOPS,   // AT.2b: P_PRECIP only
    U_HFOGPLANE, U_HFOG,                                      // AT.5: the *_HFOG twins only
    U_CRAY0, U_CRAY1, U_CRAY2, U_CPOS, U_CPARAM, U_CCOLOR, U_CSHADE,   // AT.6: P_CLOUDS only
    U_CSU, U_CSV, U_CSHADOW, U_CLOUDTEX,                               // AT.6: the *_CS twins only
    U_WTINT, U_WSUN, U_WUP,                                            // AT.8: the *_WT twins (AT.11: WSUN/WUP the *_WG twins only)
    U_SSMAT, U_SSPARAM, U_SSFADE, U_SSMAP,                             // SH.2: the SUN_SHADOW keys only
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

// PW.3: lit/lit-flat become their discard-free twin when alphaRef < 0 (unless --power-exp=discard).
ProgId alphaVariant(ProgId id, float alphaRef);
bool noDiscard(float alphaRef);       // the same test for program 1: its WF_NO_DISCARD bit
bool built(ProgId id);                // linked already (the selftest's "off = never built" check)

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
