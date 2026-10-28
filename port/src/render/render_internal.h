// render_internal.h — what the renderer core (render_core.cpp) and its two backends (gl/, null/) share.
// Not engine API: Classes/Renderer.h is. Nothing outside port/src/render/ (and the harness's selftests)
// includes this.
#ifndef EDEN_RENDER_INTERNAL_H
#define EDEN_RENDER_INTERNAL_H

#include "../../../Classes/Renderer.h"

#include <string>

namespace rcore {

// The per-draw state the last setState/setFog/setLights stated (plan R4). Backends read it at a draw.
struct DrawState {
    R::State    state = { R::B_OFF, true, true, true, 0.0f, 0.0f };
    R::Rgba     fogColor = { 0, 0, 0, 1 };
    bool        fogOn = false;
    float       fogStart = 0.0f, fogEnd = 1.0f;
    R::LightSet lights = {};
};
DrawState& ds();

// --render-selftest's invariants (plan §5). Counted always (a few integer increments); asserted only by
// the selftest, which reads and resets them.
struct Invariants {
    long frames = 0;
    long draws = 0, rdraws = 0;           // every draw / R draws (the same since the shim went, G.5b-7)
    long drawsOutsideFrame = 0;
    long nestedFrames = 0, endWithoutBegin = 0;
    long stackUnbalancedFrames = 0;       // a frame whose mv/proj/uv depth at end != at begin
    long unbalanced2D = 0, unbalancedScene = 0;
    long passUnbalancedFrames = 0;        // the pass tracker not back at depth 0 at endFrame (when tracking)
    long contractChecks = 0, contractFails = 0;
    std::string firstFailure;
};
Invariants& inv();
void fail(const char* what);              // count is the caller's; records the first failure's text
bool inFrame();
void noteDraw(bool renderer);
void setPassContracts(bool on);           // check the signs pass contract at P_R_SIGNS begin/end
bool passContracts();

}  // namespace rcore

// The GL backend (port/src/render/gl/), used by the core only while a context is live.
namespace rgl {
void beginFrame();
void endFrame();
R::Tex texCreate(const R::TexDesc& d, const void* px);
void   texUpdate(R::Tex t, int x, int y, int w, int h, R::TexFormat f, const void* px);
void   texDestroy(R::Tex t);
R::Buf bufCreate(R::BufKind k, R::Usage u);
void   bufData(R::Buf b, const void* data, size_t bytes);
void   bufSubData(R::Buf b, size_t offset, const void* data, size_t bytes);
void   bufDestroy(R::Buf b);
void   drawWorld(const R::Geometry& g, R::Tex t, const R::Rgba& tint, float alphaRef);
void   draw2D(const R::Geometry& g, R::Tex t, const R::Rgba& tint);
void   drawSprites(const R::Geometry& g, R::Tex t, const R::SpriteParams& p);
void   drawSkinned(const R::Geometry& g, R::Tex t, const R::Rgba& tint, const float* palette, int nmat,
                   int bonesPerVertex);
void   drawLit(const R::Geometry& g, R::Tex t, const R::Rgba& tint);
bool   readPixels(int x, int y, int w, int h, void* rgba);
void   texSampling(R::Tex t, const R::TexDesc& d);
void   clear(bool color, bool depth, const R::Rgba& c);
unsigned takeError();
const char* driverString(int which);      // R::DriverString
int    compileAll(std::string* log);      // the programs (five + lit-flat); returns how many linked
int    selftest(std::string* log);        // --render-selftest-gl's unit checks (gl_selftest.cpp); failures
int    litOracle(const char* outDir);     // --lit-oracle (gl_selftest.cpp): failures; -1 = no legacy GL
void   invalidateAll();                   // the context went away: every cache, ring, program, the white texture
}

// The null backend (port/src/render/null/): no context. Ids are nonzero and distinct, draws count.
namespace rnull {
R::Tex texCreate(const R::TexDesc& d);
void   texDestroy(R::Tex t);
R::Buf bufCreate();
void   bufDestroy(R::Buf b);
void   draw(const R::Geometry& g);
long   draws();
long   liveTextures();
long   liveBuffers();
}

#endif  // EDEN_RENDER_INTERNAL_H
