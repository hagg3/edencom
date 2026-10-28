// Renderer.h — the engine-owned renderer (Stage G.5; plan: WORKING/renderer-interface-plan-2026-10-25.md).
//
// Plain C++: no GL types, no GL enums. One backend per process frame: GL when a context is live, null
// otherwise (headless, failed context); both live in port/src/render/. Main thread only (root CLAUDE.md
// rule 8).
//
// G.5b-0 landed the interface, the matrix stacks (R2), the GL state cache (R3), the five programs and the
// instruments; G.5b-1…6 moved the engine onto it file group by file group, and G.5b-7 deleted the ES 1.1
// shim that served the rest: no engine or host file calls GL (port/tools/gl-call-gate.py). The engine's
// 2D code keeps its immediate-mode colour/blend/depth/texture as the pen (Classes/Graphics.h).
//
// Rules a ported call site follows (plan R4/R5):
//   * State everything per draw or per pass: setState, setFog, the texture and the tint. A renderer draw
//     reads nothing the ES 1.1 code left behind except the matrices, and leaves nothing behind for it.
//   * The colour comes from the vertex format's colour attribute when it has one (ES 1.1 semantics: the
//     array wins); `tint` is the constant colour of a format without one.
//   * Tex 0 means untextured (a 1x1 white texture is bound); alphaRef -1 means no alpha test; fogOff()
//     is fog with an end far past any eye distance. No uniform flags (R5).
#ifndef EDEN_RENDERER_H
#define EDEN_RENDERER_H

#include <cstddef>
#include <cstdint>

namespace R {

// ---- matrices (R2): column-major float[16], GL's convention ------------------------------------
struct Mat4 { float m[16]; };
void mat4Identity(Mat4& out);
void mat4Multiply(Mat4& out, const Mat4& a, const Mat4& b);   // out = a * b (b applies first)

class MatrixStack {
public:
    enum { kMaxDepth = 16 };   // ES 1.1's (the shim's) depth; a push past it or a pop at 0 is ignored (and counted)
    MatrixStack();
    const Mat4& top() const { return stack_[top_]; }
    int depth() const { return top_; }
    unsigned version() const { return version_; }   // bumps on every change: what a u_mvp cache keys on
    int overflows() const { return overflows_; }
    int underflows() const { return underflows_; }
    void push();
    void pop();
    void loadIdentity();
    void load(const float* m);
    void mult(const float* m);
    void translate(float x, float y, float z);
    void scale(float x, float y, float z);
    void rotate(float deg, float x, float y, float z);
    void ortho(float l, float r, float b, float t, float n, float f);
    void frustum(float l, float r, float b, float t, float n, float f);
private:
    Mat4 stack_[kMaxDepth];
    int top_;
    unsigned version_;
    int overflows_, underflows_;
};
MatrixStack& mv();
MatrixStack& proj();
// The texture-coordinate transform. Today's only uses are the atlas scale glScalef(1, 1/ATLAS_TILES, 1)
// and the water/lava row shift (plan §6), so programs 1 and 5 read it as one vec4 (scale.xy, offset.xy):
// uvScaleOffset(). It stays a full stack because the ES 1.1 code pushes and pops it.
MatrixStack& uv();
void uvScaleOffset(float out[4]);

// ---- geometry ----------------------------------------------------------------------------------
enum Prim { TRIANGLES, TRIANGLE_STRIP, TRIANGLE_FAN, LINES, LINE_STRIP, POINTS };
enum AttrType : uint8_t { A_NONE = 0, A_F32, A_S16, A_U8N /* unsigned byte, normalised: colours */,
                          A_U8 /* unsigned byte as is: bone indices */ };
struct Attr { uint8_t type, comps; uint16_t offset; };
// A vertex layout (the plan's `Layout`, as data): which members a vertex has and where. The engine's
// structs have named formats below; a call site with its own struct writes a static const one.
struct VertexFormat { uint16_t stride; Attr pos, color, uv, normal, size, index, weight; };
extern const VertexFormat kFmtTerrain;   // vertexStructSmall: short3 pos (+pad), ubyte4 colour, short2 uv
extern const VertexFormat kFmtObject;    // vertexObject: float3 pos, float3 normal, ubyte4 colour, float2 uv
extern const VertexFormat kFmtSprite;    // vertexpStruct: float3 pos, ubyte4 colour, float size
extern const VertexFormat kFmtBreak;     // vertexpBreak: float3 pos, float3 normal, ubyte4 colour, short2 uv
extern const VertexFormat kFmtCube;      // vertexStruct without its colour: float3 pos, float3 normal, float2 uv (tinted)
extern const VertexFormat kFmtUI2;       // float2 pos
extern const VertexFormat kFmtUI2UV;     // float2 pos, float2 uv (interleaved, 16 bytes)
extern const VertexFormat kFmtUI3UV;     // float3 pos, float2 uv (interleaved, 20 bytes): Texture2D's quads

typedef uint32_t Tex;   // opaque; 0 = none (untextured)
typedef uint32_t Buf;   // opaque; 0 = none (stream from client memory)
enum IndexType { I_NONE = 0, I_U8, I_U16, I_U32 };
struct Geometry {
    const VertexFormat* fmt;
    Buf vbuf; const void* vmem;            // vbuf 0 -> the vertices are streamed from vmem
    Buf ibuf; const void* imem; IndexType itype;   // itype I_NONE -> drawn as arrays
    int first, count;                      // arrays: vertex range; indexed: `first` index, `count` indices
    Prim prim;
};

// ---- state -------------------------------------------------------------------------------------
enum Blend { B_OFF, B_ALPHA /* SA, 1-SA */, B_ADD /* SA, 1 */, B_ONE /* 1, 1: Graphics::beginHud */ };   // the engine's three pairs
struct State {
    Blend blend;
    bool  depthTest, depthWrite, cull;
    float offsetFactor, offsetUnits;      // both 0 = polygon offset off
};
struct Rgba { float r, g, b, a; };
struct Light {                             // an ES 1.1 positional light, already in eye space
    bool  on;
    float pos[4], ambient[4], diffuse[4], specular[4];
};
struct LightSet {
    Light light[2];
    float modelAmbient[4];                 // GL_LIGHT_MODEL_AMBIENT (ES 1.1 default 0.2)
    float matSpecular[4];                  // material specular; ambient = diffuse = the colour (COLOR_MATERIAL)
    float shininess;
    bool  flat;                            // glShadeModel(GL_FLAT): a triangle takes its last vertex's colour
};
struct SpriteParams {
    float size;                            // when the format has no size attribute
    float atten[3];                        // ES 1.1 GL_POINT_DISTANCE_ATTENUATION: size / sqrt(a + b d + c d^2)
};

// ---- resources ---------------------------------------------------------------------------------
enum TexFormat { TF_RGBA8, TF_RGB8, TF_RGB565, TF_RGBA4444, TF_RGBA5551, TF_L8, TF_A8, TF_LA88 };
struct TexDesc {
    int w, h;
    TexFormat format;
    bool mipmaps;                          // a chain only when asked (P.15); min filter then blends the two nearest
                                           // levels (NEAREST_MIPMAP_LINEAR; LINEAR_MIPMAP_LINEAR when `linear`)
    bool linear;                           // false = nearest
    bool clampS, clampT;                   // false = repeat (the atlas repeats S and clamps T)
    const char* label;                     // for --mem-trace's per-texture probe (may be NULL)
};
// Uploads keep every rule the ES 1.1 shim had: NPOT (Q.2), L/A/LA as R8/RG8 + swizzle on desktop and native
// on ES (Q.4), glFlush after each upload (N.4.9), the byte accounting, the probe, --tex-exp=.
Tex  texCreate(const TexDesc& d, const void* px);
void texUpdate(Tex t, int x, int y, int w, int h, TexFormat f, const void* px);
void texDestroy(Tex t);
// Re-state a texture's sampling (filters, wrap, mip use) after creation; d.w/h/format are ignored. The
// mip chain must already exist (texCreate with mipmaps) for a mipmapped filter to be complete.
void texSampling(Tex t, const TexDesc& d);
enum BufKind { BK_VERTEX, BK_INDEX };
enum Usage { U_STATIC, U_DYNAMIC, U_STREAM };
Buf  bufCreate(BufKind k, Usage u);
void bufData(Buf b, const void* data, size_t bytes);                  // (re)allocate and fill
void bufSubData(Buf b, size_t offset, const void* data, size_t bytes);
void bufDestroy(Buf b);

// ---- frame structure ---------------------------------------------------------------------------
void beginFrame();  void endFrame();       // the host's frame (EdenViewController::drawFrame)
void beginScene();  void endScene();       // the 3D pass, offscreen at render scale < 100 % (N.4.11)
void begin2D(float w, float h, float zfar); void end2D();   // push an ortho projection + identity modelview
// Clear the target being drawn (the scene pass or the screen). A depth clear always writes depth.
// While --scene-shot masks passes, a depth clear also clears colour (pass-alone captures start clean).
void setClearColor(const Rgba& c);         // glClearColor: kept until changed (GL's default is black, alpha 0)
void clear(bool color, bool depth);

// ---- per-draw state and draws --------------------------------------------------------------------
void setState(const State& s);
void setFog(const Rgba& c, float start, float end);
void fogOff();
void setLights(const LightSet& l);
void drawWorld  (const Geometry& g, Tex t, const Rgba& tint, float alphaRef = -1.0f);   // program 1
void draw2D     (const Geometry& g, Tex t, const Rgba& tint);                          // program 2
void drawSprites(const Geometry& g, Tex t, const SpriteParams& p);                     // program 3
void drawSkinned(const Geometry& g, Tex t, const Rgba& tint, const float* palette, int nmat,
                 int bonesPerVertex);                                                  // program 4
void drawLit    (const Geometry& g, Tex t, const Rgba& tint);   // program 5, or 1 with Original lighting off (R8)

// Settings › Video › Original lighting (plan §7). G.5b-4 added the row and the first call sites (block-break
// debris, the firework cube); G.5b-6 added doors and the golden cube (Terrain::render).
bool originalLighting();
void setOriginalLighting(bool on);

// ---- readbacks ---------------------------------------------------------------------------------
// The POINT space x SCALE_* (eden_gl_set_pick_viewport), never the drawable: what picking's gluUnProject needs.
void pickViewport(int out[4]);
bool readPixels(int x, int y, int w, int h, void* rgba);   // RGBA8, bottom-up; false headless (zeroed)
void drawableSize(int& w, int& h);         // the window's drawable (the letterbox box) in pixels

// ---- diagnostics -------------------------------------------------------------------------------
unsigned takeError();                      // GL's glGetError (0 = none, and always 0 headless)
enum DriverString { DS_RENDERER, DS_VERSION, DS_EXTENSIONS };
const char* driverString(DriverString w);  // glGetString: "" headless; NULL if GL has none (core: extensions)

}  // namespace R

#endif  // EDEN_RENDERER_H
