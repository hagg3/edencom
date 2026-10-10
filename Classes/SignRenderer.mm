//
//  SignRenderer.mm
//  Eden — Stage D / D.3b. See SignRenderer.h for the look and the rules.
//
#import "SignRenderer.h"
#import "Graphics.h"
#import "Globals.h"
#import "World.h"
#import "Terrain.h"
#import "FileManager.h"
#import "Player.h"
#import "SignTool.h"
#include "WorldTrailer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

// The text-raster seam (web/src/seam/Texture2D_web.mm, native/src/seam/TextRaster_native.cpp).
// Contract: `out` is width*height*4, pre-zeroed; white RGB, coverage in A; row 0 is the top row.
extern "C" void eden_rasterize_text_rgba(const char* text, int width, int height, float fontPx,
                                         int align, unsigned char* out);
extern "C" float eden_text_raster_measure(const char* text, float fontPx);
extern "C" void eden_text_raster_set_face(int face);

namespace {

// ---- the shape, in blocks -------------------------------------------------------------------
// Boards are wider than tall, Minecraft's 2:1 (the user, 2026-10-10: the first cut's 0.75- and
// 0.875-tall boards covered the whole block face / a block's height above it).
const float kStandW = 1.0f, kStandH = 0.5f;       // the standing board
const float kStandLift = 0.5f;                    // its lower edge above the anchor's top face
const float kStandThick = 1.0f / 16.0f;
const float kPostHalf = 0.04f, kPostTop = 0.55f;  // the post: from the top face into the board
const float kWallHalfW = 7.0f / 16.0f;            // inset 1/16 from the face's side edges
const float kWallHalfH = 0.25f;                   // half a block tall, centred on the face
const float kWallOut = 1.0f / 32.0f;              // the board's front, off the face
const float kPanelHalf = 0.375f;                  // a command block's inner panel (1/8 frame)
const float kTextLift = 0.004f;                   // text in front of its board

// ---- the command-block skin (D.4b) ----------------------------------------------------------
// Every exposed face of a block with a CMB1 record: an orange frame, a charcoal panel inside it
// and a square button, red idle / green while its script runs. Fixed colours, never the voxel's
// paint. Frame and panel lie flat on the steel (kSkinOut plus a polygon offset: at P_ZNEAR 0.012
// a 24-bit depth step is ~0.02 block at CMD_RADIUS, so distance alone would z-fight); the button
// is a small box standing out past a wall sign's board, so a sign never hides it.
const float kSkinOut = 0.002f;
const float kButtonHalf = 0.14f;
const float kButtonOut = kWallOut + 0.016f;
const float kButtonInOut = kWallOut + 0.005f;    // pressed (running, D.4c): pushed in, still over a board
const float kFrameRGB[3] = {0.95f, 0.52f, 0.10f};
const float kPanelRGB[3] = {0.17f, 0.17f, 0.18f};
const float kButtonIdleRGB[3] = {0.90f, 0.08f, 0.06f};
const float kButtonRunRGB[3] = {0.12f, 0.80f, 0.18f};

// ---- the text raster ------------------------------------------------------------------------
const int kTexW = 256;                            // texels across the board, whatever its width
enum Kind { K_STAND = 0, K_WALL = 1, K_CMD = 2 };
const float kFontPx[3] = {22.0f, 30.0f, 28.0f};   // per board kind, at kTexW across the board
const float kBoardW[3] = {kStandW, 2 * kWallHalfW, 2 * kPanelHalf};
const float kBoardH[3] = {kStandH, 2 * kWallHalfH, 2 * kPanelHalf};
const unsigned char kOutline[3] = {16, 16, 16};

struct TextTex {
    GLuint name;
    int h;          // texels used, from the top (the texture is kTexW x pot(h))
    int potH;
    int bytes;
    unsigned lastFrame;
};

float measure(const std::string& s, float px) {
    float w = eden_text_raster_measure(s.c_str(), px);
    if (w <= 0.0f) w = (float)s.size() * px * 0.5f;   // headless web has no canvas: estimate
    return w;
}

// Greedy word wrap; a word wider than the line is broken by characters.
void wrap(const char* text, float px, float maxW, std::vector<std::string>& out) {
    out.clear();
    std::vector<std::string> words;
    std::string w;
    for (const char* p = text; *p; ++p) {
        if (*p == ' ') { if (!w.empty()) { words.push_back(w); w.clear(); } }
        else w += *p;
    }
    if (!w.empty()) words.push_back(w);
    std::string line;
    for (size_t i = 0; i < words.size(); ++i) {
        std::string word = words[i];
        std::string cand = line.empty() ? word : line + " " + word;
        if (measure(cand, px) <= maxW) { line = cand; continue; }
        if (!line.empty()) { out.push_back(line); line.clear(); }
        while (measure(word, px) > maxW && word.size() > 1) {
            size_t n = word.size() - 1;
            while (n > 1 && measure(word.substr(0, n), px) > maxW) --n;
            out.push_back(word.substr(0, n));
            word = word.substr(n);
        }
        line = word;
    }
    if (!line.empty()) out.push_back(line);
}

int pot(int v) { int p = 1; while (p < v) p <<= 1; return p; }

// Lays out and rasterises one board's text: white glyphs, a dark outline, transparent elsewhere
// (with the outline's RGB, so mip filtering never pulls in a white fringe).
bool buildText(const char* text, int kind, TextTex* t) {
    const float boardTexH = kTexW * kBoardH[kind] / kBoardW[kind];
    const float margin = kTexW * 0.04f;
    float px = kFontPx[kind];
    std::vector<std::string> lines;
    eden_text_raster_set_face(0);
    for (;;) {
        wrap(text, px, kTexW - 2 * margin, lines);
        const float need = margin + lines.size() * px * 1.2f + margin;
        if (need <= boardTexH || px <= 9.0f) break;
        px *= 0.9f;   // shrink only when the text would overflow the board
    }
    if (lines.empty()) return false;
    const int lineH = (int)std::ceil(px * 1.2f);
    const int top = (int)margin / 2;
    int h = top + (int)lines.size() * lineH + 4;
    if (h > (int)boardTexH) h = (int)boardTexH;
    const int potH = pot(h);
    std::vector<unsigned char> cov((size_t)kTexW * potH, 0);
    std::vector<unsigned char> buf((size_t)kTexW * lineH * 4);
    for (size_t i = 0; i < lines.size(); ++i) {
        std::fill(buf.begin(), buf.end(), 0);
        eden_rasterize_text_rgba(lines[i].c_str(), kTexW, lineH, px, 1, buf.data());
        const int y0 = top + (int)i * lineH;
        for (int y = 0; y < lineH && y0 + y < potH; ++y)
            for (int x = 0; x < kTexW; ++x)
                cov[(size_t)(y0 + y) * kTexW + x] = buf[((size_t)y * kTexW + x) * 4 + 3];
    }
    // The outline: the coverage dilated by r texels (a disc), composited under the glyphs.
    const int r = px >= 18.0f ? 2 : 1;
    std::vector<unsigned char> rgba((size_t)kTexW * potH * 4);
    for (int y = 0; y < potH; ++y)
        for (int x = 0; x < kTexW; ++x) {
            int d = 0;
            for (int dy = -r; dy <= r; ++dy) {
                const int yy = y + dy;
                if (yy < 0 || yy >= potH) continue;
                for (int dx = -r; dx <= r; ++dx) {
                    const int xx = x + dx;
                    if (xx < 0 || xx >= kTexW || dx * dx + dy * dy > r * r + 1) continue;
                    const int c = cov[(size_t)yy * kTexW + xx];
                    if (c > d) d = c;
                }
            }
            const float a = cov[(size_t)y * kTexW + x] / 255.0f;
            const float o = d / 255.0f;
            const float outA = a + o * (1.0f - a);
            unsigned char* p = &rgba[((size_t)y * kTexW + x) * 4];
            for (int k = 0; k < 3; ++k) {
                const float c = outA > 0.0f ? (255.0f * a + kOutline[k] * o * (1.0f - a)) / outA
                                            : kOutline[k];
                p[k] = (unsigned char)(c + 0.5f);
            }
            p[3] = (unsigned char)(outA * 255.0f + 0.5f);
        }
    GLint saved = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved);
    glGenTextures(1, &t->name);
    if (!t->name) return false;
    glBindTexture(GL_TEXTURE_2D, t->name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kTexW, potH, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, (GLuint)saved);
    t->h = h;
    t->potH = potH;
    t->bytes = kTexW * potH * 4 * 4 / 3;
    return true;
}

// ---- the cache ------------------------------------------------------------------------------
std::unordered_map<std::string, TextTex> g_cache;
int g_cacheBytes = 0;
unsigned g_frame = 0;
SignRenderer::Stats g_stats;
char g_describe[384];

std::string cacheKey(int kind, const char* text) { return std::string(1, (char)('0' + kind)) + text; }

// Drops least-recently-used textures not drawn this frame until `need` more bytes and one more
// entry fit. False when everything cached is in use this frame (the caller draws no text).
bool makeRoom(int need) {
    while ((int)g_cache.size() >= SignRenderer::MAX_TEXTURES ||
           g_cacheBytes + need > SignRenderer::MAX_TEX_BYTES) {
        auto victim = g_cache.end();
        for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
            if (it->second.lastFrame != g_frame &&
                (victim == g_cache.end() || it->second.lastFrame < victim->second.lastFrame))
                victim = it;
        if (victim == g_cache.end()) return false;
        glDeleteTextures(1, &victim->second.name);
        g_cacheBytes -= victim->second.bytes;
        g_cache.erase(victim);
    }
    return true;
}

// ---- geometry -------------------------------------------------------------------------------
struct V3 { float x, y, z; };
V3 v3(float x, float y, float z) { V3 v = {x, y, z}; return v; }
V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
V3 mul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }

std::vector<vertexObject> g_flat;   // boards, posts, buttons: untextured, vertex colours
std::vector<vertexObject> g_skin;   // command-block frames + panels: flat on the face, offset
std::vector<vertexObject> g_text;   // one 6-vertex quad per textured sign
std::vector<GLuint> g_textTex;      // ...and its texture
GLuint g_vbo = 0;
int g_vboCap = 0;

void vert(std::vector<vertexObject>& out, V3 p, float s, float t, const float rgb[3]) {
    vertexObject v;
    memset(&v, 0, sizeof(v));
    v.position[0] = p.x; v.position[1] = p.y; v.position[2] = p.z;
    v.texs[0] = s; v.texs[1] = t;
    v.colors[0] = (GLubyte)(rgb[0] * 255.0f);
    v.colors[1] = (GLubyte)(rgb[1] * 255.0f);
    v.colors[2] = (GLubyte)(rgb[2] * 255.0f);
    v.colors[3] = 255;
    out.push_back(v);
}
// A quad centred on c spanning +-r and +-u; (s,t) runs 0..s1 left to right, 0..t1 top to bottom.
void quad(std::vector<vertexObject>& out, V3 c, V3 r, V3 u, const float rgb[3],
          float s1 = 0, float t1 = 0) {
    const V3 tl = add(add(c, mul(r, -1)), u), tr = add(add(c, r), u);
    const V3 bl = add(add(c, mul(r, -1)), mul(u, -1)), br = add(add(c, r), mul(u, -1));
    vert(out, tl, 0, 0, rgb);  vert(out, bl, 0, t1, rgb); vert(out, br, s1, t1, rgb);
    vert(out, tl, 0, 0, rgb);  vert(out, br, s1, t1, rgb); vert(out, tr, s1, 0, rgb);
}
// A box: front face at c + n*d (the colour as given), the rest a little darker for definition.
void box(std::vector<vertexObject>& out, V3 c, V3 r, V3 u, V3 n, float hr, float hu, float d,
         const float rgb[3]) {
    const float dark[3] = {rgb[0] * 0.75f, rgb[1] * 0.75f, rgb[2] * 0.75f};
    const V3 R = mul(r, hr), U = mul(u, hu), D = mul(n, d);
    quad(out, add(c, D), R, U, rgb);
    quad(out, add(c, mul(D, -1)), mul(R, -1), U, dark);
    quad(out, add(c, R), D, U, dark);
    quad(out, add(c, mul(R, -1)), mul(D, -1), U, dark);
    quad(out, add(c, U), R, mul(D, -1), dark);
    quad(out, add(c, mul(U, -1)), R, D, dark);
}

V3 dirOf(int quarter) {   // c's quarter turns, engine axes: 0 -x, 1 -z, 2 +x, 3 +z
    switch (quarter & 3) {
        case 0: return v3(-1, 0, 0);
        case 1: return v3(0, 0, -1);
        case 2: return v3(1, 0, 0);
        default: return v3(0, 0, 1);
    }
}
bool faceNormal(int a, V3* n) {   // a's faces, engine axes; false for bottom / out of range
    switch (a) {
        case 0: *n = v3(-1, 0, 0); return true;
        case 1: *n = v3(1, 0, 0); return true;
        case 4: *n = v3(0, 0, -1); return true;
        case 5: *n = v3(0, 0, 1); return true;
        default: return false;
    }
}

bool opaque(int t) { return t > 0 && t <= NUM_BLOCKS && !(blockinfo[t] & IS_NOTSOLID); }

// The six faces of a block for the skin: the outward normal, the face's in-plane axes, the
// trailer's face code (a) for a wall sign on it, and a fixed shade so the cube reads as one.
struct SkinFace { V3 n, r, u; int a; float shade; };
const SkinFace kSkinFaces[6] = {
    {{-1, 0, 0}, {0, 0, -1}, {0, 1, 0}, 0, 0.80f},
    {{ 1, 0, 0}, {0, 0,  1}, {0, 1, 0}, 1, 0.80f},
    {{ 0, 1, 0}, {1, 0,  0}, {0, 0, -1}, 3, 1.00f},
    {{ 0,-1, 0}, {1, 0,  0}, {0, 0,  1}, 2, 0.55f},
    {{ 0, 0,-1}, {1, 0,  0}, {0, 1, 0}, 4, 0.70f},
    {{ 0, 0, 1}, {-1, 0, 0}, {0, 1, 0}, 5, 0.70f},
};

void shaded(const float in[3], float k, float out[3]) { out[0] = in[0] * k; out[1] = in[1] * k; out[2] = in[2] * k; }

// One command block's skin: frame ring + panel into g_skin, the button box into g_flat.
void skinBlock(Terrain* terrain, int ex, int ey, int ez, V3 centre, bool running,
               SignRenderer::Stats* st) {
    const float frame = 0.5f, panel = kPanelHalf;
    const float ringW = (frame - panel) * 0.5f, ringC = (frame + panel) * 0.5f;
    for (int f = 0; f < 6; ++f) {
        const SkinFace& F = kSkinFaces[f];
        const int nx = ex + (int)F.n.x, ny = ey + (int)F.n.y, nz = ez + (int)F.n.z;
        if (ny >= 0 && ny < T_HEIGHT && opaque(terrain->getLand(nx, nz, ny))) continue;
        st->cmdFaces++;
        float fr[3], pn[3], bt[3];
        shaded(kFrameRGB, F.shade, fr);
        shaded(kPanelRGB, F.shade, pn);
        shaded(running ? kButtonRunRGB : kButtonIdleRGB, F.shade, bt);
        const V3 c = add(centre, mul(F.n, 0.5f + kSkinOut));
        // The frame as four strips (not a square under the panel), so the two never overlap.
        quad(g_skin, add(c, mul(F.u, ringC)), mul(F.r, frame), mul(F.u, ringW), fr);
        quad(g_skin, add(c, mul(F.u, -ringC)), mul(F.r, frame), mul(F.u, ringW), fr);
        quad(g_skin, add(c, mul(F.r, ringC)), mul(F.r, ringW), mul(F.u, panel), fr);
        quad(g_skin, add(c, mul(F.r, -ringC)), mul(F.r, ringW), mul(F.u, panel), fr);
        quad(g_skin, c, mul(F.r, panel), mul(F.u, panel), pn);
        // The button: a box from the face out to kButtonOut -- in front of a wall sign's board --
        // and pushed in to kButtonInOut while the script runs (the 2026 game depresses it too).
        const float out = running ? kButtonInOut : kButtonOut;
        const V3 bc = add(centre, mul(F.n, 0.5f + out * 0.5f));
        box(g_flat, bc, F.r, F.u, F.n, kButtonHalf, kButtonHalf, out * 0.5f, bt);
    }
}

struct Candidate { int index; float d2; };

}  // namespace

bool SignRenderer::enabled = true;

const SignRenderer::Stats& SignRenderer::stats() { return g_stats; }

const char* SignRenderer::describe() {
    const Stats& s = g_stats;
    snprintf(g_describe, sizeof(g_describe),
             "{\"records\":%d,\"inRange\":%d,\"drawn\":%d,\"hiddenAir\":%d,\"hiddenFront\":%d,"
             "\"onCmd\":%d,\"withText\":%d,\"textures\":%d,\"textureBytes\":%d,"
             "\"cmds\":%d,\"cmdInRange\":%d,\"cmdFaces\":%d,\"cmdRunning\":%d}",
             s.records, s.inRange, s.drawn, s.hiddenAir, s.hiddenFront, s.onCmd, s.withText,
             s.textures, s.textureBytes, s.cmds, s.cmdInRange, s.cmdFaces, s.cmdRunning);
    return g_describe;
}

void SignRenderer::releaseTextures() {
    for (auto& e : g_cache) glDeleteTextures(1, &e.second.name);
    g_cache.clear();
    g_cacheBytes = 0;
}

void SignRenderer::render() {
    World* world = World::getWorld;
    memset(&g_stats, 0, sizeof(g_stats));
    if (!enabled || !world || !world->fm || !world->terrain || !world->player) return;
    WorldTrailer* tr = world->fm->trailer();
    g_stats.records = (int)tr->signCount();
    g_stats.cmds = (int)tr->cmdCount();
    g_stats.textures = (int)g_cache.size();
    g_stats.textureBytes = g_cacheBytes;
    if (tr->signCount() == 0 && tr->cmdCount() == 0) return;
    ++g_frame;

    Terrain* terrain = world->terrain;
    const Vector pp = world->player->pos;
    const float offX = (float)(world->fm->chunkOffsetX * CHUNK_SIZE);
    const float offZ = (float)(world->fm->chunkOffsetZ * CHUNK_SIZE);
    const float r2 = (float)RADIUS * RADIUS;

    // Pass 1: which signs are in range and visible, nearest first (the texture budget's order).
    std::vector<Candidate> cand;
    for (size_t i = 0; i < tr->signCount(); ++i) {
        const TrailerSign& s = tr->sign(i);
        int ex, ey, ez;
        s.pos().toEngine(&ex, &ey, &ez);
        const float dx = ex + 0.5f - pp.x, dy = ey + 0.5f - pp.y, dz = ez + 0.5f - pp.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > r2) continue;
        if (s.a != 3 && !(s.a == 0 || s.a == 1 || s.a == 4 || s.a == 5)) continue;
        if (ey < 0 || ey >= T_HEIGHT) continue;
        g_stats.inRange++;
        const int anchor = terrain->getLand(ex, ez, ey);
        if (anchor <= 0) { g_stats.hiddenAir++; continue; }
        V3 n = v3(0, 1, 0);
        if (s.a != 3) faceNormal(s.a, &n);
        const int front = terrain->getLand(ex + (int)n.x, ez + (int)n.z, ey + (int)n.y);
        if (opaque(front)) { g_stats.hiddenFront++; continue; }
        Candidate c = {(int)i, d2};
        cand.push_back(c);
    }
    std::sort(cand.begin(), cand.end(), [](const Candidate& a, const Candidate& b) { return a.d2 < b.d2; });

    g_flat.clear();
    g_skin.clear();
    g_text.clear();
    g_textTex.clear();

    // D.4b: the command-block skins, out to CMD_RADIUS (cheap: no textures). Hidden when the
    // anchor is air, like a sign; never deleted here.
    const float c2 = (float)CMD_RADIUS * CMD_RADIUS;
    for (size_t i = 0; i < tr->cmdCount(); ++i) {
        const TrailerCmd& cb = tr->cmd(i);
        int ex, ey, ez;
        cb.pos().toEngine(&ex, &ey, &ez);
        if (ey < 0 || ey >= T_HEIGHT) continue;
        const float dx = ex + 0.5f - pp.x, dy = ey + 0.5f - pp.y, dz = ez + 0.5f - pp.z;
        if (dx * dx + dy * dy + dz * dz > c2) continue;
        if (terrain->getLand(ex, ez, ey) <= 0) continue;
        g_stats.cmdInRange++;
        const bool running = CmdTool::running(cb.pos());
        if (running) g_stats.cmdRunning++;
        skinBlock(terrain, ex, ey, ez, v3(ex + 0.5f - offX, ey + 0.5f, ez + 0.5f - offZ), running, &g_stats);
    }

    const float postRGB[3] = {0.42f, 0.29f, 0.16f};
    extern Vector colorTable[256];
    for (const Candidate& c : cand) {
        const TrailerSign& s = tr->sign((size_t)c.index);
        int ex, ey, ez;
        s.pos().toEngine(&ex, &ey, &ez);
        const V3 centre = v3(ex + 0.5f - offX, ey + 0.5f, ez + 0.5f - offZ);
        const Vector bc = colorTable[(s.b >= 1 && s.b <= 54) ? s.b : 45];
        const float rgb[3] = {bc.x, bc.y, bc.z};
        const V3 up = v3(0, 1, 0);
        V3 n, front, faceC;
        int kind;
        if (s.a == 3) {
            kind = K_STAND;
            n = dirOf(s.c);
            const V3 top = add(centre, v3(0, 0.5f, 0));
            const V3 bcen = add(top, v3(0, kStandLift + kStandH * 0.5f, 0));
            // right = (-n) x up: what a reader facing the text sees as "right"
            const V3 right = v3(n.z, 0, -n.x);
            box(g_flat, bcen, right, up, n, kStandW * 0.5f, kStandH * 0.5f, kStandThick * 0.5f, rgb);
            box(g_flat, add(top, v3(0, kPostTop * 0.5f, 0)), right, up, n, kPostHalf, kPostTop * 0.5f,
                kPostHalf, postRGB);
            front = add(bcen, mul(n, kStandThick * 0.5f));
        } else {
            faceNormal(s.a, &n);
            const bool cmd = tr->findCmd(s.pos()) >= 0;
            kind = cmd ? K_CMD : K_WALL;
            faceC = add(centre, mul(n, 0.5f));
            const V3 right = v3(n.z, 0, -n.x);
            // On a command block the board fills the skin's panel; the skin pass draws the button
            // in front of it (kButtonOut > kWallOut).
            const float hw = cmd ? kPanelHalf : kWallHalfW, hh = cmd ? kPanelHalf : kWallHalfH;
            const V3 bcen = add(faceC, mul(n, kWallOut * 0.5f));
            box(g_flat, bcen, right, up, n, hw, hh, kWallOut * 0.5f, rgb);
            front = add(faceC, mul(n, kWallOut));
            if (cmd) g_stats.onCmd++;
        }
        g_stats.drawn++;

        // The text: cached, else built if this frame's budget allows.
        if (!s.text[0]) continue;
        char text[96];
        memcpy(text, s.text, 95);
        text[95] = 0;
        const std::string key = cacheKey(kind, text);
        auto it = g_cache.find(key);
        if (it == g_cache.end()) {
            if (g_stats.built >= BUILDS_PER_FRAME) continue;
            if (!makeRoom(kTexW * 64 * 4 * 4 / 3)) continue;
            TextTex t;
            memset(&t, 0, sizeof(t));
            g_stats.built++;
            if (!buildText(text, kind, &t)) continue;
            if (g_cacheBytes + t.bytes > MAX_TEX_BYTES && !makeRoom(t.bytes)) {
                glDeleteTextures(1, &t.name);
                continue;
            }
            it = g_cache.insert(std::make_pair(key, t)).first;
            g_cacheBytes += t.bytes;
        }
        it->second.lastFrame = g_frame;
        const V3 right = v3(n.z, 0, -n.x);
        const float bw = kBoardW[kind], bh = kBoardH[kind];
        const float th = bw * it->second.h / (float)kTexW;              // text height in blocks
        const V3 boardTop = add(front, v3(0, bh * 0.5f, 0));
        const V3 tc = add(add(boardTop, v3(0, -th * 0.5f, 0)), mul(n, kTextLift));
        const float white[3] = {1, 1, 1};
        quad(g_text, tc, mul(right, bw * 0.5f), mul(up, th * 0.5f), white, 1.0f,
             it->second.h / (float)it->second.potH);
        g_textTex.push_back(it->second.name);
        g_stats.withText++;
    }
    g_stats.textures = (int)g_cache.size();
    g_stats.textureBytes = g_cacheBytes;

    // Draw. State on entry (after Terrain::render + RenderModels): texture on, lighting off,
    // blend off, cull on, GL_ARRAY_BUFFER 0, colour array off. Left exactly so on exit.
    // Buffer layout: [skin | flat | text].
    const size_t total = g_skin.size() + g_flat.size() + g_text.size();
    if (!total) return;
    if (!g_vbo) glGenBuffers(1, &g_vbo);
    if (!g_vbo) return;
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    if ((int)total > g_vboCap) {
        int cap = g_vboCap ? g_vboCap : 1024;
        while (cap < (int)total) cap *= 2;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)cap * sizeof(vertexObject), NULL, GL_DYNAMIC_DRAW);
        g_vboCap = cap;
    }
    const size_t flat0 = g_skin.size(), text0 = g_skin.size() + g_flat.size();
    if (!g_skin.empty())
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(g_skin.size() * sizeof(vertexObject)), g_skin.data());
    if (!g_flat.empty())
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(flat0 * sizeof(vertexObject)),
                        (GLsizeiptr)(g_flat.size() * sizeof(vertexObject)), g_flat.data());
    if (!g_text.empty())
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(text0 * sizeof(vertexObject)),
                        (GLsizeiptr)(g_text.size() * sizeof(vertexObject)), g_text.data());
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, sizeof(vertexObject), (const void*)offsetof(vertexObject, position));
    glTexCoordPointer(2, GL_FLOAT, sizeof(vertexObject), (const void*)offsetof(vertexObject, texs));
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(vertexObject), (const void*)offsetof(vertexObject, colors));
    glDisable(GL_CULL_FACE);
    glDisable(GL_TEXTURE_2D);
    if (!g_skin.empty()) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -4.0f);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_skin.size());
        glPolygonOffset(0.0f, 0.0f);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    if (!g_flat.empty()) glDrawArrays(GL_TRIANGLES, (GLint)flat0, (GLsizei)g_flat.size());
    glEnable(GL_TEXTURE_2D);
    if (!g_text.empty()) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.02f);
        for (size_t i = 0; i < g_textTex.size(); ++i) {
            glBindTexture(GL_TEXTURE_2D, g_textTex[i]);
            glDrawArrays(GL_TRIANGLES, (GLint)(text0 + i * 6), 6);
        }
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_BLEND);
    }
    glEnable(GL_CULL_FACE);
    glDisableClientState(GL_COLOR_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}
