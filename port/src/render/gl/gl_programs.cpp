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
    "uniform vec3 u_worldLight;\n"   // DN.5: R::setWorldLight; (1,1,1) unless the day/night cycle is on (x1.0 is exact)
    "in vec4 a_position;\n"
    "in vec4 a_color;\n"
    "in vec2 a_texcoord;\n"
    "out vec4 v_color;\n"
    "out vec2 v_texcoord;\n"
    "out float v_eyedist;\n"
    "#ifdef EDEN_HEIGHT_FOG\n"   // AT.5 (plan P3): the High tier's twin only; the plain program never sees it
    "uniform vec4 u_hfogPlane;\n"
    "out float v_wy;\n"
    "#endif\n"
    "#ifdef EDEN_CLOUD_SHADOW\n"   // AT.6 (plan P3): the High tier's cloud shadows, per vertex (block corners)
    "uniform vec4 u_csU;\n"
    "uniform vec4 u_csV;\n"
    "uniform vec4 u_cshadow;\n"   // threshold, strength
    "uniform sampler2D u_cloudTex;\n"   // texture unit 1
    "#endif\n"
    "#ifdef EDEN_WATER_TINT\n"   // AT.8: water's vertices only (alpha 145/255), tinted
    "uniform vec4 u_wtint;\n"    // rgb: the colour water is pulled toward, w: how far
    "#endif\n"
    "#ifdef EDEN_WATER_GLINT\n"  // AT.11: + the High tier's sun glint (a faint sheen here, sparkles in the fragment stage)
    "uniform vec4 u_wsun;\n"     // xyz: eye-space direction to the sun, w: glint strength
    "uniform vec4 u_wup;\n"      // xyz: world up in eye space (unit), w: the sparkle clock
    "out highp vec2 v_sp;\n"     // the sparkle cell (1/16 block) of the surface
    "out float v_glint;\n"       // how near the mirror direction (0 off water)
    "#endif\n"
    "void main() {\n"
    "  gl_Position = u_mvp * a_position;\n"
    "  vec4 eye = u_mv * a_position;\n"
    "  v_eyedist = length(eye.xyz);\n"
    "  v_color = a_color * vec4(u_worldLight, 1.0);\n"
    "  v_texcoord = a_texcoord * u_uvso.xy + u_uvso.zw;\n"
    "#ifdef EDEN_HEIGHT_FOG\n"
    "  v_wy = dot(u_hfogPlane.xyz, eye.xyz) + u_hfogPlane.w;\n"
    "#endif\n"
    "#ifdef EDEN_CLOUD_SHADOW\n"
    "  vec2 cuv = vec2(dot(u_csU.xyz, eye.xyz) + u_csU.w, dot(u_csV.xyz, eye.xyz) + u_csV.w);\n"
    "  float cv = textureLod(u_cloudTex, cuv, 0.0).r;\n"
    "  v_color.rgb *= 1.0 - u_cshadow.y * step(u_cshadow.x, cv);\n"
    "#endif\n"
    "#ifdef EDEN_WATER_TINT\n"
    "  bool water = abs(a_color.a - 0.5686) < 0.004;\n"
    "  if (water) v_color.rgb = mix(v_color.rgb, u_wtint.rgb, u_wtint.w);\n"
    "#ifdef EDEN_WATER_GLINT\n"
    "  v_sp = a_position.xz * 4.0;\n"   // terrain positions are in quarter blocks: cells of 1/16 block
    "  v_glint = 0.0;\n"
    "  if (water) {\n"
    "    vec3 r = reflect(normalize(eye.xyz), u_wup.xyz);\n"
    "    float gl = max(dot(r, u_wsun.xyz), 0.0);\n"
    "    gl *= gl; gl *= gl; gl *= gl; gl *= gl;\n"  // ^16: the sun's path on the water
    "    v_glint = gl;\n"
    "    gl *= gl;\n"                                // ^32
    "    v_color.rgb += u_wsun.w * 0.3 * gl;\n"      // a faint sheen right at the mirror direction
    "  }\n"
    "#endif\n"
    "#endif\n"
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
    "#ifdef EDEN_HEIGHT_FOG\n"
    "uniform vec4 u_hfog;\n"     // AT.5: mist top y, 1 / thickness, 1 / distance to full, strength
    "in float v_wy;\n"
    "#endif\n"
    "#ifdef EDEN_WATER_GLINT\n"
    "uniform vec4 u_wsun;\n"
    "uniform vec4 u_wup;\n"
    "in highp vec2 v_sp;\n"
    "in float v_glint;\n"
    "#endif\n"
    "void main() {\n"
    "  vec4 c = v_color * texture(u_tex, EDEN_UV(v_texcoord));\n"
    "#ifdef EDEN_WATER_GLINT\n"   // AT.11: texel-sized sparkles along the sun's path, each twinkling on its own phase
    "  highp vec2 cell = floor(v_sp);\n"
    "  highp float h = fract(sin(dot(cell, vec2(12.9898, 78.233))) * 43758.5453);\n"
    "  float tw = fract(h * 13.7 + u_wup.w);\n"
    "  float sp = step(1.0 - 0.1 * v_glint, h) * step(tw, 0.3);\n"
    "  c.rgb = mix(c.rgb, vec3(1.0, 0.97, 0.86), clamp(sp * u_wsun.w * 1.8, 0.0, 1.0));\n"
    "#endif\n"
    "#ifndef EDEN_NO_DISCARD\n"   // PW.3: the opaque variants never discard, so the GPU keeps early-Z/HSR
    "  if (c.a <= u_alphaRef) discard;\n"
    "#endif\n"
    "  float f = clamp((u_fogEnd - v_eyedist) / u_fogRange, 0.0, 1.0);\n"
    "#ifdef EDEN_HEIGHT_FOG\n"   // denser the deeper below the mist top, and the farther away
    "  f = min(f, 1.0 - u_hfog.w * clamp((u_hfog.x - v_wy) * u_hfog.y, 0.0, 1.0) * clamp(v_eyedist * u_hfog.z, 0.0, 1.0));\n"
    "#endif\n"
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

// AT.2b (atmosphere plan P1, P2): rain streaks. a_position is a static seed (x, y, z in 0..255) and a
// corner (bit 0 the side, bit 1 the tail end); six vertices a streak. The drop's head is the seed placed
// in the box and wrapped (mod) around the camera-centred box, so drops are fixed in the world and only the
// box's edges move; the tail leans up-wind. A head below its column's top (u_tex, an L8 n x n map by
// (x, z) mod n, read in the vertex stage) collapses the streak to a point outside the clip volume. No
// discard, no fog (the box is 32 blocks across), one mediump multiply in the fragment stage.
static const char* kPrecipVS =
    "uniform mat4 u_mv;\n"
    "uniform mat4 u_proj;\n"
    "uniform vec4 u_pbox0;\n"    // origin.xyz, fade start
    "uniform vec4 u_pbox1;\n"    // size.xyz, fade end
    "uniform vec4 u_pmove;\n"    // offset.xyz, streak length (splash mode: the phase clock)
    "uniform vec4 u_pslant;\n"   // slant.xz, width, width per eye distance
    "uniform vec4 u_pcolor;\n"
    "uniform vec4 u_ptops;\n"    // shift.xz, n, mode (0 streaks, 1 splashes: AT.2c)
    "uniform sampler2D u_tex;\n"
    "in vec4 a_position;\n"
    "out vec4 v_color;\n"
    "out vec3 v_side;\n"
    "void main() {\n"
    "  float side = mod(a_position.w, 2.0) * 2.0 - 1.0;\n"
    "  float tail = floor(a_position.w * 0.5);\n"
    "  if (u_ptops.w > 0.5 && u_ptops.w < 1.5) {\n"
    // AT.2c: a splash is a flat quad on its column's top. The seed's x/z pick a column (fixed in the world,
    // as a drop's), its y a phase in the cycle; it shows for the first quarter of the cycle, a ring growing
    // while it fades. A column with no top (0) shows none.
    "    vec2 c = u_pbox0.xz + mod(a_position.xz * (1.0 / 256.0) * u_pbox1.xz - u_pbox0.xz, u_pbox1.xz);\n"
    "    vec2 cell = mod(floor(c) + u_ptops.xy, u_ptops.z);\n"
    "    float top = textureLod(u_tex, (cell + 0.5) / u_ptops.z, 0.0).r * 255.0;\n"
    "    float ph = fract(a_position.y * (1.0 / 256.0) + u_pmove.w);\n"
    "    float t = ph * 4.0;\n"
    "    float alive = step(ph, 0.25) * step(0.5, top);\n"
    "    float rad = 0.10 + 0.28 * t;\n"
    "    vec3 p = vec3(floor(c.x) + 0.5, top + 0.04, floor(c.y) + 0.5) + vec3(side, 0.0, tail * 2.0 - 1.0) * rad;\n"
    "    vec3 e = (u_mv * vec4(p, 1.0)).xyz;\n"
    "    float r = length(c - (u_pbox0.xz + 0.5 * u_pbox1.xz));\n"
    "    float fade = (1.0 - smoothstep(u_pbox0.w, u_pbox1.w, r)) * (1.0 - t);\n"
    "    v_color = vec4(u_pcolor.rgb, u_pcolor.a * fade);\n"
    "    v_side = vec3(side, tail * 2.0 - 1.0, 1.0);\n"
    "    gl_Position = mix(vec4(0.0, 0.0, -2.0, 1.0), u_proj * vec4(e, 1.0), alive);\n"
    "    return;\n"
    "  }\n"
    "  vec3 p = u_pbox0.xyz + mod(a_position.xyz * (1.0 / 256.0) * u_pbox1.xyz + u_pmove.xyz - u_pbox0.xyz, u_pbox1.xyz);\n"
    // AT.3: a flake is a camera-facing square at the head (1x or 2x by seed), swaying as it falls.
    "  if (u_ptops.w > 1.5) {\n"
    "    p.x += 0.35 * sin(u_pmove.y * 0.9 + a_position.x);\n"
    "    p.z += 0.35 * cos(u_pmove.y * 0.7 + a_position.z);\n"
    "    vec2 cell = mod(floor(p.xz) + u_ptops.xy, u_ptops.z);\n"
    "    float top = textureLod(u_tex, (cell + 0.5) / u_ptops.z, 0.0).r * 255.0;\n"
    "    float alive = step(top, p.y);\n"
    "    vec3 eh = (u_mv * vec4(p, 1.0)).xyz;\n"
    "    float sz = min(max(u_pslant.z, u_pslant.w * length(eh)), 2.0 * u_pslant.w * length(eh)) * (1.0 + step(128.0, a_position.y));\n"
    "    vec3 e = eh + vec3(side, tail * 2.0 - 1.0, 0.0) * (0.5 * sz);\n"
    "    float r = length(p.xz - (u_pbox0.xz + 0.5 * u_pbox1.xz));\n"
    "    float h = (p.y - u_pbox0.y) / u_pbox1.y;\n"
    "    float fade = (1.0 - smoothstep(u_pbox0.w, u_pbox1.w, r)) * (1.0 - smoothstep(0.75, 1.0, h)) * smoothstep(0.0, 0.1, h);\n"
    "    v_color = vec4(u_pcolor.rgb, u_pcolor.a * fade);\n"
    "    v_side = vec3(0.0);\n"
    "    gl_Position = mix(vec4(0.0, 0.0, -2.0, 1.0), u_proj * vec4(e, 1.0), alive);\n"
    "    return;\n"
    "  }\n"
    "  vec3 q = p + vec3(-u_pslant.x, 1.0, -u_pslant.y) * u_pmove.w;\n"
    "  vec2 cell = mod(floor(p.xz) + u_ptops.xy, u_ptops.z);\n"
    "  float top = textureLod(u_tex, (cell + 0.5) / u_ptops.z, 0.0).r * 255.0;\n"
    "  float alive = step(top, p.y);\n"
    "  vec3 eh = (u_mv * vec4(p, 1.0)).xyz;\n"
    "  vec3 et = (u_mv * vec4(q, 1.0)).xyz;\n"
    "  vec3 e = mix(eh, et, tail);\n"
    "  vec3 across = cross(et - eh, e);\n"
    "  across *= inversesqrt(max(dot(across, across), 1e-12));\n"
    "  e += across * (side * 0.5 * min(max(u_pslant.z, u_pslant.w * length(e)), 2.0 * u_pslant.w * length(e)));\n"
    "  float r = length(p.xz - (u_pbox0.xz + 0.5 * u_pbox1.xz));\n"
    "  float h = (p.y - u_pbox0.y) / u_pbox1.y;\n"
    "  float fade = (1.0 - smoothstep(u_pbox0.w, u_pbox1.w, r)) * (1.0 - smoothstep(0.75, 1.0, h)) * smoothstep(0.0, 0.1, h);\n"
    "  v_color = vec4(u_pcolor.rgb, u_pcolor.a * fade);\n"
    "  v_side = vec3(side, 0.0, 0.0);\n"
    "  gl_Position = mix(vec4(0.0, 0.0, -2.0, 1.0), u_proj * vec4(e, 1.0), alive);\n"
    "}\n";
// AT.6 (atmosphere plan P5): the pixel cloud layer. One quad over the whole viewport at a clip depth just
// nearer than the sky's, drawn after the sky with a depth test, so terrain hides it for free. Each fragment
// intersects its view ray (affine in NDC, so interpolated exactly) with the cloud plane and samples the
// tileable cloud map there, nearest: whole texels are cloud or sky (the pixel look). highp throughout: the
// hit point is up to a few thousand blocks out.
static const char* kCloudsVS =
    "uniform vec4 u_cray0;\n"   // the ray at NDC (0, 0); w = the clip depth
    "uniform vec4 u_cray1;\n"   // d ray / d x
    "uniform vec4 u_cray2;\n"   // d ray / d y
    "in vec4 a_position;\n"
    "out highp vec3 v_ray;\n"
    "void main() {\n"
    "  v_ray = u_cray0.xyz + a_position.x * u_cray1.xyz + a_position.y * u_cray2.xyz;\n"
    "  gl_Position = vec4(a_position.xy, u_cray0.w, 1.0);\n"
    "}\n";
static const char* kCloudsFS =
    "precision highp float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec4 u_cpos;\n"     // eye x, z (minus the wind, wrapped), plane height above the eye, 1 / period
    "uniform vec4 u_cparam;\n"   // threshold, fade start, fade end
    "uniform vec4 u_ccolor;\n"   // lit rgb, alpha
    "uniform vec4 u_cshade;\n"   // shade rgb
    "in highp vec3 v_ray;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  float dy = abs(v_ray.y) < 1e-6 ? 1e-6 : v_ray.y;\n"
    "  float t = u_cpos.z / dy;\n"
    "  vec2 off = v_ray.xz * t;\n"
    "  float v = textureLod(u_tex, (u_cpos.xy + off) * u_cpos.w, 0.0).r;\n"
    "  float a = step(0.0, t) * step(u_cparam.x, v) * (1.0 - smoothstep(u_cparam.y, u_cparam.z, length(off)));\n"
    "  vec3 c = mix(u_ccolor.rgb, u_cshade.rgb, smoothstep(u_cparam.x, 1.0, v));\n"
    "  fragColor = vec4(c, u_ccolor.a * a);\n"
    "}\n";

static const char* kPrecipFS =
    "precision mediump float;\n"
    "in vec4 v_color;\n"
    "in vec3 v_side;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    // AT.11: a splash (v_side.z = 1) is a hard-edged square ring, 4 x 4 cells across (the pixel look), not a
    // soft blob; a streak or flake keeps its soft sides.
    "  vec2 q = floor(abs(v_side.xy) * 3.999);\n"
    "  float ring = step(2.5, max(q.x, q.y));\n"
    "  float soft = (1.0 - abs(v_side.x)) * (1.0 - abs(v_side.y));\n"
    "  fragColor = vec4(v_color.rgb, v_color.a * mix(soft, ring, v_side.z));\n"
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
    {"precip", kPrecipVS, kPrecipFS, kSmooth},   // AT.2b
    // AT.5 (P3): program 1 with the height-fog term, for the High tier's mist; built on first use.
    {"world-hfog", kWorldVS, kWorldFS, "#define EDEN_COLOR_QUAL\n#define EDEN_HEIGHT_FOG\n"},
    {"world-opaque-hfog", kWorldVS, kWorldFS, "#define EDEN_COLOR_QUAL\n#define EDEN_NO_DISCARD\n#define EDEN_HEIGHT_FOG\n"},
    {"clouds", kCloudsVS, kCloudsFS, kSmooth},   // AT.6
    // AT.6 (P3): the cloud-shadow twins of the four world programs above (a vertex-stage term only).
    {"world-cs", kWorldVS, kWorldFS, "#define EDEN_COLOR_QUAL\n#define EDEN_CLOUD_SHADOW\n"},
    {"world-opaque-cs", kWorldVS, kWorldFS, "#define EDEN_COLOR_QUAL\n#define EDEN_NO_DISCARD\n#define EDEN_CLOUD_SHADOW\n"},
    {"world-hfog-cs", kWorldVS, kWorldFS, "#define EDEN_COLOR_QUAL\n#define EDEN_HEIGHT_FOG\n#define EDEN_CLOUD_SHADOW\n"},
    {"world-opaque-hfog-cs", kWorldVS, kWorldFS,
     "#define EDEN_COLOR_QUAL\n#define EDEN_NO_DISCARD\n#define EDEN_HEIGHT_FOG\n#define EDEN_CLOUD_SHADOW\n"},
    // AT.8: the water-tint twins of the four discard-free world programs (the transparent chunk draws).
    {"world-opaque-wt", kWorldVS, kWorldFS, "#define EDEN_COLOR_QUAL\n#define EDEN_NO_DISCARD\n#define EDEN_WATER_TINT\n"},
    {"world-opaque-hfog-wt", kWorldVS, kWorldFS,
     "#define EDEN_COLOR_QUAL\n#define EDEN_NO_DISCARD\n#define EDEN_HEIGHT_FOG\n#define EDEN_WATER_TINT\n"},
    {"world-opaque-cs-wt", kWorldVS, kWorldFS,
     "#define EDEN_COLOR_QUAL\n#define EDEN_NO_DISCARD\n#define EDEN_CLOUD_SHADOW\n#define EDEN_WATER_TINT\n"},
    {"world-opaque-hfog-cs-wt", kWorldVS, kWorldFS,
     "#define EDEN_COLOR_QUAL\n#define EDEN_NO_DISCARD\n#define EDEN_HEIGHT_FOG\n#define EDEN_CLOUD_SHADOW\n#define EDEN_WATER_TINT\n"},
    // AT.11: the glint twins of the four water-tint twins (High: sparkles in the fragment stage, water draws only).
    {"world-opaque-wg", kWorldVS, kWorldFS,
     "#define EDEN_COLOR_QUAL\n#define EDEN_NO_DISCARD\n#define EDEN_WATER_TINT\n#define EDEN_WATER_GLINT\n"},
    {"world-opaque-hfog-wg", kWorldVS, kWorldFS,
     "#define EDEN_COLOR_QUAL\n#define EDEN_NO_DISCARD\n#define EDEN_HEIGHT_FOG\n#define EDEN_WATER_TINT\n#define EDEN_WATER_GLINT\n"},
    {"world-opaque-cs-wg", kWorldVS, kWorldFS,
     "#define EDEN_COLOR_QUAL\n#define EDEN_NO_DISCARD\n#define EDEN_CLOUD_SHADOW\n#define EDEN_WATER_TINT\n#define EDEN_WATER_GLINT\n"},
    {"world-opaque-hfog-cs-wg", kWorldVS, kWorldFS,
     "#define EDEN_COLOR_QUAL\n#define EDEN_NO_DISCARD\n#define EDEN_HEIGHT_FOG\n#define EDEN_CLOUD_SHADOW\n#define EDEN_WATER_TINT\n#define EDEN_WATER_GLINT\n"},
};

static const char* const kUniName[U_COUNT] = {
    "u_mvp", "u_mv", "u_proj", "u_uvso", "u_tex",
    "u_fogColor", "u_fogEnd", "u_fogRange", "u_alphaRef",
    "u_pointAtten",
    "u_palette[0]", "u_boneCount", "u_lightAmbient", "u_lightDiffuse",
    "u_ambSum", "u_lpos0", "u_lpos1", "u_ldiff0", "u_ldiff1", "u_lspec0", "u_lspec1", "u_shininess",
    "u_worldLight",
    "u_pbox0", "u_pbox1", "u_pmove", "u_pslant", "u_pcolor", "u_ptops",
    "u_hfogPlane", "u_hfog",
    "u_cray0", "u_cray1", "u_cray2", "u_cpos", "u_cparam", "u_ccolor", "u_cshade",
    "u_csU", "u_csV", "u_cshadow", "u_cloudTex",
    "u_wtint", "u_wsun", "u_wup",
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

// AT.5: the height-fog twin of a world program (P_WORLD or P_WORLD_OPAQUE); anything else unchanged.
ProgId heightFogVariant(ProgId id) {
    return id == P_WORLD ? P_WORLD_HFOG : id == P_WORLD_OPAQUE ? P_WORLD_OPAQUE_HFOG : id;
}

// AT.8: the water-tint twin of a discard-free world program (plain, height-fog, cloud-shadow or both).
ProgId waterTintVariant(ProgId id) {
    switch (id) {
        case P_WORLD_OPAQUE: return P_WORLD_OPAQUE_WT;
        case P_WORLD_OPAQUE_HFOG: return P_WORLD_OPAQUE_HFOG_WT;
        case P_WORLD_OPAQUE_CS: return P_WORLD_OPAQUE_CS_WT;
        case P_WORLD_OPAQUE_HFOG_CS: return P_WORLD_OPAQUE_HFOG_CS_WT;
        default: return id;
    }
}

// AT.11: the glint twin of a water-tint twin; anything else unchanged.
ProgId waterGlintVariant(ProgId id) {
    switch (id) {
        case P_WORLD_OPAQUE_WT: return P_WORLD_OPAQUE_WG;
        case P_WORLD_OPAQUE_HFOG_WT: return P_WORLD_OPAQUE_HFOG_WG;
        case P_WORLD_OPAQUE_CS_WT: return P_WORLD_OPAQUE_CS_WG;
        case P_WORLD_OPAQUE_HFOG_CS_WT: return P_WORLD_OPAQUE_HFOG_CS_WG;
        default: return id;
    }
}

// AT.6: the cloud-shadow twin of a world program (plain or height-fog); anything else unchanged.
ProgId cloudShadowVariant(ProgId id) {
    switch (id) {
        case P_WORLD: return P_WORLD_CS;
        case P_WORLD_OPAQUE: return P_WORLD_OPAQUE_CS;
        case P_WORLD_HFOG: return P_WORLD_HFOG_CS;
        case P_WORLD_OPAQUE_HFOG: return P_WORLD_OPAQUE_HFOG_CS;
        default: return id;
    }
}

bool built(ProgId id) { return g_progs[id].id != 0; }

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
