// gl_programs.cpp — see gl_programs.h. The preamble and compile step moved verbatim from
// gl_fixed_function.cpp; the five programs are new (plan §6). Their arithmetic copies the shim's
// expression for expression (eye distance, fog, alpha test, point attenuation, skinning) so a draw
// moved from the shim to R:: keeps its pixels: --render-selftest-gl compares the two on one FBO.
#include "gl_programs.h"

#include <cstdio>
#include <cstring>

extern "C" int eden_gl_tex_experiments(void);   // gl_textures.cpp, --tex-exp=
enum { EDEN_TEX_EXP_FP16UV = 4 };

namespace rgl {

// Phase N Stage 1 / 4.1: the shader sources are shared between ES and desktop core; the dialects
// disagree about spelling, not semantics, so a per-context preamble is prepended. The switch is
// "is the CONTEXT ES" (EDEN_GL_ES_DIALECT), not the compiler: iOS is not Emscripten and is ES 3.0.
const char* shaderPreamble(GLenum stage, bool es300) {
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

GLuint compileShader(GLenum stage, const char* src, bool es300, const char* defs) {
    GLuint sh = glCreateShader(stage);
    const char* parts[3] = { shaderPreamble(stage, es300), defs ? defs : "", src };
    glShaderSource(sh, 3, parts, nullptr);
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

GLuint linkProgram(GLuint vs, GLuint fs, const char* name, std::string* log) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    // Attribute locations are BOUND, not queried, so they equal the ATTR_* slot indices in every
    // program: the attribute setup uses the slot as the location with no lookup table.
    bindAttribLocations(prog);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[1024] = {0};
        glGetProgramInfoLog(prog, sizeof(buf) - 1, nullptr, buf);
        std::fprintf(stderr, "[eden-gl] %s program link failed:\n%s\n", name, buf);
        if (log) { *log += name; *log += ": "; *log += buf; *log += "\n"; }
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// ---- the five programs (GLSL ES 3.00 / 330 core bodies) ------------------------------------------

// 1 world: chunks, objects, flowers, sky, effects quads, signs. Fog by data: fogOff() is end 1e4,
// range 1, so f is exactly 1.0 for any eye distance the window reaches.
static const char* kWorldVS =
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_mv;\n"
    "uniform vec4 u_uvso;\n"
    "in vec4 a_position;\n"
    "in vec4 a_color;\n"
    "in vec2 a_texcoord;\n"
    "out vec4 v_color;\n"
    "out vec2 v_texcoord;\n"
    "out float v_eyedist;\n"
    "void main() {\n"
    "  gl_Position = u_mvp * a_position;\n"
    "  vec4 eye = u_mv * a_position;\n"
    "  v_eyedist = length(eye.xyz);\n"
    "  v_color = a_color;\n"
    "  v_texcoord = a_texcoord * u_uvso.xy + u_uvso.zw;\n"
    "}\n";
static const char* kWorldFS =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_fogColor;\n"
    "uniform float u_fogEnd;\n"
    "uniform float u_fogRange;\n"
    "uniform float u_alphaRef;\n"
    "EDEN_COLOR_QUAL in vec4 v_color;\n"
    "in highp vec2 v_texcoord;\n"
    "in float v_eyedist;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  vec4 c = v_color * texture(u_tex, EDEN_UV(v_texcoord));\n"
    "#ifndef EDEN_NO_DISCARD\n"   // PW.3: the opaque variants never discard, so the GPU keeps early-Z/HSR
    "  if (c.a <= u_alphaRef) discard;\n"
    "#endif\n"
    "  float f = clamp((u_fogEnd - v_eyedist) / u_fogRange, 0.0, 1.0);\n"
    "  c.rgb = mix(u_fogColor.rgb, c.rgb, f);\n"
    "  fragColor = c;\n"
    "}\n";

// 2 ui: the HUD, menus, text. No fog, no texture transform.
static const char* kUiVS =
    "uniform mat4 u_mvp;\n"
    "in vec4 a_position;\n"
    "in vec4 a_color;\n"
    "in vec2 a_texcoord;\n"
    "out vec4 v_color;\n"
    "out vec2 v_texcoord;\n"
    "void main() {\n"
    "  gl_Position = u_mvp * a_position;\n"
    "  v_color = a_color;\n"
    "  v_texcoord = a_texcoord;\n"
    "}\n";
static const char* kUiFS =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "in vec4 v_color;\n"
    "in highp vec2 v_texcoord;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  fragColor = v_color * texture(u_tex, EDEN_UV(v_texcoord));\n"
    "}\n";

// 3 sprite: point particles. ES 1.1's size / sqrt(a + b d + c d^2), the shim's expression; the size
// is the attribute, or a constant attribute when the format has none. UV is gl_PointCoord.
static const char* kSpriteVS =
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_mv;\n"
    "uniform vec3 u_pointAtten;\n"
    "in vec4 a_position;\n"
    "in vec4 a_color;\n"
    "in float a_pointsize;\n"
    "out vec4 v_color;\n"
    "out float v_eyedist;\n"
    "void main() {\n"
    "  gl_Position = u_mvp * a_position;\n"
    "  vec4 eye = u_mv * a_position;\n"
    "  v_eyedist = length(eye.xyz);\n"
    "  v_color = a_color;\n"
    "  float d = v_eyedist;\n"
    "  float att = sqrt(max(u_pointAtten.x + u_pointAtten.y*d + u_pointAtten.z*d*d, 1e-6));\n"
    "  gl_PointSize = a_pointsize / att;\n"
    "}\n";
static const char* kSpriteFS =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_fogColor;\n"
    "uniform float u_fogEnd;\n"
    "uniform float u_fogRange;\n"
    "in vec4 v_color;\n"
    "in float v_eyedist;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  vec4 c = v_color * texture(u_tex, gl_PointCoord);\n"
    "  float f = clamp((u_fogEnd - v_eyedist) / u_fogRange, 0.0, 1.0);\n"
    "  c.rgb = mix(u_fogColor.rgb, c.rgb, f);\n"
    "  fragColor = c;\n"
    "}\n";

// 4 skinned: the shim's matrix-palette program without its flags. The palette maps object space
// straight to eye space (Model.cpp loads m_mView * m_mTransform * bone), hence u_proj, not u_mvp.
static const char* kSkinnedVS =
    "uniform mat4 u_proj;\n"
    "uniform mat4 u_palette[16];\n"
    "uniform vec4 u_uvso;\n"
    "uniform int  u_boneCount;\n"
    "uniform vec3 u_lightAmbient;\n"
    "uniform vec3 u_lightDiffuse;\n"
    "in vec4 a_position;\n"
    "in vec4 a_color;\n"
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
    "  for (int b = 0; b < 4; ++b) {\n"
    "    if (b >= u_boneCount) break;\n"
    "    float w = a_weight[b];\n"
    "    int idx = clamp(int(a_matrixindex[b] + 0.5), 0, 15);\n"
    "    skin += u_palette[idx] * w;\n"
    "    wsum += w;\n"
    "  }\n"
    "  if (wsum <= 0.0001) skin = u_palette[0];\n"
    "  vec4 eye = skin * a_position;\n"
    "  gl_Position = u_proj * eye;\n"
    "  v_eyedist = length(eye.xyz);\n"
    "  vec3 n = normalize(mat3(skin) * a_normal);\n"
    "  vec3 L = normalize(-eye.xyz);\n"
    "  float ndotl = max(dot(n, L), 0.0);\n"
    "  vec3 lit = u_lightAmbient + u_lightDiffuse * ndotl;\n"
    "  v_color = vec4(clamp(a_color.rgb * lit, 0.0, 1.0), a_color.a);\n"
    "  v_texcoord = a_texcoord * u_uvso.xy + u_uvso.zw;\n"
    "}\n";
static const char* kSkinnedFS =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_fogColor;\n"
    "uniform float u_fogEnd;\n"
    "uniform float u_fogRange;\n"
    "in vec4 v_color;\n"
    "in highp vec2 v_texcoord;\n"
    "in float v_eyedist;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  vec4 c = v_color * texture(u_tex, EDEN_UV(v_texcoord));\n"
    "  float f = clamp((u_fogEnd - v_eyedist) / u_fogRange, 0.0, 1.0);\n"
    "  c.rgb = mix(u_fogColor.rgb, c.rgb, f);\n"
    "  fragColor = c;\n"
    "}\n";

// 5 lit: ES 1.1's per-vertex lighting for doors and the golden cube (plan §6, G.5b-4 turns it on).
// GL_COLOR_MATERIAL: material ambient = diffuse = the vertex colour; emission 0; no attenuation;
// non-local viewer (h = L + (0,0,1)). u_ambSum = model ambient + every enabled light's ambient (CPU).
// A disabled light has zero diffuse and specular and a harmless direction.
static const char* kLitVS =
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_mv;\n"
    "uniform vec4 u_uvso;\n"
    "uniform vec3 u_ambSum;\n"
    "uniform vec4 u_lpos0;\n"
    "uniform vec4 u_lpos1;\n"
    "uniform vec3 u_ldiff0;\n"
    "uniform vec3 u_ldiff1;\n"
    "uniform vec3 u_lspec0;\n"
    "uniform vec3 u_lspec1;\n"
    "uniform float u_shininess;\n"
    "in vec4 a_position;\n"
    "in vec4 a_color;\n"
    "in vec2 a_texcoord;\n"
    "in vec3 a_normal;\n"
    "EDEN_COLOR_QUAL out vec4 v_color;\n"
    "out vec2 v_texcoord;\n"
    "out float v_eyedist;\n"
    "float spec(vec3 n, vec3 L, float ndotl) {\n"
    "  if (ndotl <= 0.0) return 0.0;\n"
    "  float nh = max(dot(n, normalize(L + vec3(0.0, 0.0, 1.0))), 0.0);\n"
    "  return u_shininess > 0.0 ? pow(nh, u_shininess) : 1.0;\n"
    "}\n"
    "void main() {\n"
    "  gl_Position = u_mvp * a_position;\n"
    "  vec4 eye = u_mv * a_position;\n"
    "  v_eyedist = length(eye.xyz);\n"
    "  vec3 n = normalize(mat3(u_mv) * a_normal);\n"
    "  vec3 L0 = normalize(u_lpos0.xyz - eye.xyz * u_lpos0.w);\n"
    "  vec3 L1 = normalize(u_lpos1.xyz - eye.xyz * u_lpos1.w);\n"
    "  float d0 = max(dot(n, L0), 0.0);\n"
    "  float d1 = max(dot(n, L1), 0.0);\n"
    "  vec3 lit = u_ambSum + u_ldiff0 * d0 + u_ldiff1 * d1;\n"
    "  vec3 rgb = a_color.rgb * lit + u_lspec0 * spec(n, L0, d0) + u_lspec1 * spec(n, L1, d1);\n"
    "  v_color = vec4(clamp(rgb, 0.0, 1.0), a_color.a);\n"
    "  v_texcoord = a_texcoord * u_uvso.xy + u_uvso.zw;\n"
    "}\n";

// I.1/FT.8: the texcoord varyings are `highp` in every fragment stage. Under `precision mediump`
// they were fp16 on Apple GPUs (A18 measured by eye: iPhone 16 Pro Max), and the block atlas is
// 32x4096 (128 tiles of 32 rows) whose tile UVs are inset only 1e-5 (0.04 row): fp16 rounds v by up
// to 0.25 row at tile 25 (cloud) and 1 row past v = 0.5, so a tile's edge sampled its neighbour's
// row = a 1 px strip of another block. Desktop GL is fp32 either way, so the Mac never showed it.
// --tex-exp=fp16uv defines EDEN_UV_FP16, which rounds the texcoord to fp16 (10-bit mantissa) the
// way the mediump varying did: the Mac repro of the old iOS look.
static const char* kUvFS =
    "#ifdef EDEN_UV_FP16\n"
    "highp vec2 eden_fp16(highp vec2 t) {\n"
    "  highp vec2 e = exp2(floor(log2(max(abs(t), vec2(6.103515625e-5)))));\n"
    "  return floor(t / e * 1024.0 + 0.5) / 1024.0 * e;\n"
    "}\n"
    "#define EDEN_UV(t) eden_fp16(t)\n"
    "#else\n"
    "#define EDEN_UV(t) (t)\n"
    "#endif\n";

// `defs` follows the preamble: EDEN_COLOR_QUAL is the colour varying's interpolation qualifier.
struct ProgSrc { const char* name; const char* vs; const char* fs; const char* defs; };
static const char* kSmooth = "#define EDEN_COLOR_QUAL\n";
static const ProgSrc kProgSrc[P_PROG_COUNT] = {
    {"world", kWorldVS, kWorldFS, kSmooth},
    {"ui", kUiVS, kUiFS, kSmooth},
    {"sprite", kSpriteVS, kSpriteFS, kSmooth},
    {"skinned", kSkinnedVS, kSkinnedFS, kSmooth},
    {"lit", kLitVS, kWorldFS, kSmooth},   // lit shares the world fragment stage (texture, alpha, fog)
    {"lit-flat", kLitVS, kWorldFS, "#define EDEN_COLOR_QUAL flat\n"},
    // PW.3 (G1): the same fragment stage with the alpha test compiled out, for draws with alphaRef < 0.
    {"world-opaque", kWorldVS, kWorldFS, "#define EDEN_COLOR_QUAL\n#define EDEN_NO_DISCARD\n"},
    {"lit-opaque", kLitVS, kWorldFS, "#define EDEN_COLOR_QUAL\n#define EDEN_NO_DISCARD\n"},
    {"lit-flat-opaque", kLitVS, kWorldFS, "#define EDEN_COLOR_QUAL flat\n#define EDEN_NO_DISCARD\n"},
};

static const char* const kUniName[U_COUNT] = {
    "u_mvp", "u_mv", "u_proj", "u_uvso", "u_tex",
    "u_fogColor", "u_fogEnd", "u_fogRange", "u_alphaRef",
    "u_pointAtten",
    "u_palette[0]", "u_boneCount", "u_lightAmbient", "u_lightDiffuse",
    "u_ambSum", "u_lpos0", "u_lpos1", "u_ldiff0", "u_ldiff1", "u_lspec0", "u_lspec1", "u_shininess",
};

static Program g_progs[P_PROG_COUNT];

static bool build(ProgId id, std::string* log) {
    Program& p = g_progs[id];
    p = Program();
    p.tried = true;
    const ProgSrc& s = kProgSrc[id];
    std::string fsDefs = s.defs;
    if (eden_gl_tex_experiments() & EDEN_TEX_EXP_FP16UV) fsDefs += "#define EDEN_UV_FP16\n";
    fsDefs += kUvFS;
    GLuint vs = compileShader(GL_VERTEX_SHADER, s.vs, true, s.defs);
    GLuint fs = vs ? compileShader(GL_FRAGMENT_SHADER, s.fs, true, fsDefs.c_str()) : 0;
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (log) { *log += s.name; *log += ": compile failed\n"; }
        return false;
    }
    p.id = linkProgram(vs, fs, s.name, log);
    if (!p.id) return false;
    for (int u = 0; u < U_COUNT; ++u) { p.loc[u] = glGetUniformLocation(p.id, kUniName[u]); p.valid[u] = false; }
    return true;
}

extern "C" int eden_power_exp(int bit);   // power.cpp
ProgId alphaVariant(ProgId id, float alphaRef) {
    if (alphaRef >= 0.0f || eden_power_exp(32768 /* EDEN_PEXP_DISCARD, harness.h */)) return id;
    switch (id) {
        case P_WORLD: return P_WORLD_OPAQUE;
        case P_LIT: return P_LIT_OPAQUE;
        case P_LIT_FLAT: return P_LIT_FLAT_OPAQUE;
        default: return id;
    }
}

Program* program(ProgId id) {
    Program& p = g_progs[id];
    if (!p.tried) build(id, nullptr);
    return p.id ? &p : nullptr;
}

int compileAll(std::string* log) {
    int n = 0;
    for (int i = 0; i < P_PROG_COUNT; ++i) {
        if (g_progs[i].id) { ++n; continue; }
        if (build((ProgId)i, log)) ++n;
    }
    return n;
}

void invalidatePrograms() {
    for (int i = 0; i < P_PROG_COUNT; ++i) g_progs[i] = Program();
}

// Each setter sends only a change (the shim's item-#4 rule), and counts the call either way.
static bool changed(Program& p, Uni u, const float* v, int n) {
    if (p.loc[u] < 0) return false;
    if (p.valid[u] && std::memcmp(p.cache[u], v, sizeof(float) * n) == 0) { stat_elided(); return false; }
    std::memcpy(p.cache[u], v, sizeof(float) * n);
    p.valid[u] = true;
    stat_issued();
    return true;
}
void set1f(Program& p, Uni u, float v) { if (changed(p, u, &v, 1)) glUniform1f(p.loc[u], v); }
void set1i(Program& p, Uni u, int v) {
    float f = (float)v;
    if (changed(p, u, &f, 1)) glUniform1i(p.loc[u], v);
}
void set3f(Program& p, Uni u, const float* v) { if (changed(p, u, v, 3)) glUniform3fv(p.loc[u], 1, v); }
void set4f(Program& p, Uni u, const float* v) { if (changed(p, u, v, 4)) glUniform4fv(p.loc[u], 1, v); }
void setMat4(Program& p, Uni u, const float* m) {
    if (changed(p, u, m, 16)) glUniformMatrix4fv(p.loc[u], 1, GL_FALSE, m);
}
void setPalette(Program& p, const float* m, int n) {
    if (p.loc[U_PALETTE] < 0 || n <= 0) return;
    if (n > 16) n = 16;
    if (p.valid[U_PALETTE] && p.paletteN == n &&
        std::memcmp(p.paletteCache, m, sizeof(float) * 16 * n) == 0) { stat_elided(); return; }
    std::memcpy(p.paletteCache, m, sizeof(float) * 16 * n);
    p.paletteN = n;
    p.valid[U_PALETTE] = true;
    glUniformMatrix4fv(p.loc[U_PALETTE], n, GL_FALSE, m);
    stat_issued();
}

}  // namespace rgl
