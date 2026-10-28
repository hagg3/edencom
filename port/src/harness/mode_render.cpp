// G.5b-0: mode group "render" — the engine-owned renderer's instruments
// (WORKING/renderer-interface-plan-2026-10-25.md §9).
//   --scene-shot=PREFIX   a REAL WINDOW: a scratch world holding every object kind, fixed etime, the RNG
//                         pinned, three camera poses x day/night, the full frame plus each pass alone
//                         (render_passes.h's mask). PREFIX-<day|night>-p<N>-<pass>.bmp, compared
//                         between two builds by port/tools/shot-diff.py.
//   --state-trace=FILE    with --scene-shot: one line per draw for one full frame (day, pose 0).
#include "harness.h"
#include "../render/render_passes.h"
#include "../render/render_internal.h"
#include "../../../Classes/SpecialEffects.h"
#include "../../../Classes/Firework.h"

extern "C" void eden_gl_trace_begin(FILE* f);   // render_core.cpp
extern "C" void eden_gl_trace_end(void);
extern "C" void eden_gl_stats_frame_boundary(void);
extern "C" const char* eden_debug_model_state(void);   // port/src/seam/DebugState.cpp

using namespace harness;

namespace harness {
namespace g_render {

namespace {

World* W() { return World::getWorld; }
Terrain* T() { return World::getWorld->terrain; }

// The top solid block of column (x, z) at or below `from`, or -1.
int ground(int x, int z, int from) {
    for (int y = from; y >= 0; --y) if (T()->getLand(x, z, y) > 0) return y;
    return -1;
}

uint64_t fnv(const std::vector<unsigned char>& px) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char b : px) { h ^= b; h *= 1099511628211ULL; }
    return h;
}

// One render-only frame (no World::update) with `mask`, read back and written as PREFIX-label.bmp.
// Every capture of one pose renders the SAME world state, so the pass captures are slices of the
// full one. The back buffer is read before any present (capture()'s rule).
bool shot(const std::string& label, unsigned mask) {
    int w = 0, h = 0;
    eden_gl_context_get_drawable_size(&w, &h);
    if (w <= 0 || h <= 0) { std::fprintf(stderr, "[eden-scene] no drawable\n"); return false; }
    eden_render_set_draw_mask(mask);
    eden_gl_stats_frame_boundary();
    R::beginFrame();
    W()->renderFrame(true);
    R::endFrame();
    std::vector<unsigned char> px((size_t)w * h * 4);
    R::readPixels(0, 0, w, h, px.data());
    eden_render_set_draw_mask(EDEN_RC_ALL);
    const std::string path = g_opt.shot + "-" + label + ".bmp";
    if (!g_shots::write_bmp(path.c_str(), w, h, px)) return false;
    std::printf("[eden-scene] %-28s %016llx  (%dx%d)\n", label.c_str(), (unsigned long long)fnv(px), w, h);
    std::fflush(stdout);
    return true;
}

struct Site { int x0, z0, g; };

void place(int x, int z, int y, int type) { eden_console_setblock(x, z, y, type); }
void build(int x, int z, int y, int type) { eden_debug_build_block(x, z, y, type); }

// The static scene. Terrain argument order (x, z, y) throughout (root CLAUDE.md rule 1).
Site build_site(const PlayerState& ps) {
    Site s;
    s.x0 = (int)std::floor(ps.x) + 4;
    s.z0 = (int)std::floor(ps.z) - 6;
    s.g = ground(s.x0 + 6, s.z0 + 6, g_world_height - 12);
    if (s.g < 4) s.g = 4;
    const int G = s.g;
    for (int x = s.x0 - 1; x <= s.x0 + 12; ++x)
        for (int z = s.z0 - 1; z <= s.z0 + 12; ++z) {
            for (int y = g_world_height - 1; y > G; --y)
                if (T()->getLand(x, z, y) != TYPE_NONE) place(x, z, y, TYPE_NONE);
            place(x, z, G, TYPE_STONE);
            place(x, z, G - 1, TYPE_STONE);
        }
    tick(4);
    // Liquids in 2x2 pits in the floor.
    for (int i = 0; i < 2; ++i) for (int k = 0; k < 2; ++k) {
        place(s.x0 + 1 + i, s.z0 + 1 + k, G, TYPE_WATER);
        place(s.x0 + 1 + i, s.z0 + 4 + k, G, TYPE_LAVA);
    }
    // Glass and leaves.
    place(s.x0 + 4, s.z0 + 1, G + 1, TYPE_GLASS);
    place(s.x0 + 5, s.z0 + 1, G + 1, TYPE_LEAVES);
    place(s.x0 + 4, s.z0 + 2, G + 1, TYPE_GLASS);
    // Two doors (the near one opens as the camera comes close), a portal, golden cubes, flowers.
    build(s.x0 + 1, s.z0 + 8, G + 1, TYPE_DOOR_TOP);
    build(s.x0 + 8, s.z0 + 10, G + 1, TYPE_DOOR_TOP);
    build(s.x0 + 9, s.z0 + 4, G + 1, TYPE_PORTAL_TOP);
    build(s.x0 + 6, s.z0 + 2, G + 1, TYPE_GOLDEN_CUBE);
    build(s.x0 + 7, s.z0 + 2, G + 1, TYPE_GOLDEN_CUBE);
    eden_debug_light_edit(2, s.x0 + 7, s.z0 + 2, G + 1, 12);   // the painted one
    for (int i = 0; i < 3; ++i) place(s.x0 + 3 + i, s.z0 + 6, G + 1, TYPE_FLOWER);
    // Signs: a standing one on a stone post's top, a wall one on its side; a command block.
    place(s.x0 + 6, s.z0 + 8, G + 1, TYPE_STONE);
    SignTool::use(s.x0 + 6, s.z0 + 8, G + 1, 0, 1, 0, 180.0f);
    SignTool::answer(0, "Scene shot");
    if (GLDialog::active()) GLDialog::dismiss();
    SignTool::use(s.x0 + 6, s.z0 + 8, G + 1, -1, 0, 0, 180.0f);
    SignTool::answer(0, "Wall sign");
    if (GLDialog::active()) GLDialog::dismiss();
    place(s.x0 + 10, s.z0 + 7, G + 1, TYPE_STONE);
    CmdTool::use(s.x0 + 10, s.z0 + 7, G + 1, 0, 1, 0);
    if (GLDialog::active()) GLDialog::dismiss();
    return s;
}

// The dynamic objects, re-made from the same RNG state for each time of day.
void spawn_dynamic(const Site& s) {
    const int G = s.g;
    for (int t = 0; t < NUM_CREATURES; ++t)
        SpawnCreatureAt(t, MakeVector(s.x0 + 2.5f + 1.2f * t, G + 1.5f, s.z0 + 10.5f));
    place(s.x0 + 10, s.z0 + 2, G + 1, TYPE_WOOD);
    T()->burnBlock(s.x0 + 10, s.z0 + 2, G + 1, false);
    T()->fireworks->addFirework(s.x0 + 11, G + 2, s.z0 + 10, 5);
    W()->effects->addBlockBreak(s.x0 + 3, s.z0 + 3, G + 1, TYPE_BRICK, 0);
}

struct Pose { float dx, dy, dz, yaw, pitch; };
const Pose kPoses[3] = {
    { -4.0f, 4.0f,  6.0f,   0.0f, -15.0f },   // from -x, the whole site
    {  6.0f, 6.0f, -4.0f,  90.0f, -25.0f },   // from -z, higher
    {  3.0f, 3.0f,  7.0f,  60.0f, -10.0f },   // close, among the objects (near door open)
};

void hold_pose(const Site& s, const Pose& p, int frames) {
    Player* pl = W()->player;
    for (int k = 0; k < frames; ++k) {
        eden_console_teleport(s.x0 + p.dx, s.g + p.dy, s.z0 + p.dz);
        pl->yaw = p.yaw; pl->pitch = p.pitch;
        tick(1);
    }
    eden_console_teleport(s.x0 + p.dx, s.g + p.dy, s.z0 + p.dz);
    pl->yaw = p.yaw; pl->pitch = p.pitch;
}

const unsigned kPassBits[] = { EDEN_RC_CHUNKS, EDEN_RC_OBJECTS, EDEN_RC_SKY, EDEN_RC_MODELS, EDEN_RC_SIGNS,
                               EDEN_RC_TRANSPARENT, EDEN_RC_FLOWERS, EDEN_RC_EFFECTS, EDEN_RC_HUD, EDEN_RC_OTHER };

}  // namespace

int run_scene_shot() {
    SDL_GL_SetSwapInterval(0);
    if (g_opt.shot.empty()) g_opt.shot = "scene";
    const std::string name = g_opt.world.empty() ? "scene-shot" : g_opt.world;
    // A leftover scratch world would carry its edits and creatures into this run.
    for (int i = 0, n = eden_menu_world_count(); i < n; ++i)
        if (name == eden_menu_world_name(i)) { eden_menu_delete_at(i); break; }
    g_pass.track = true;
    g_ignoreUserInput = true;
    g_app->viewController.setFixedEtime(1.0f / 60.0f);
    eden_rng_seed(1);
    if (!open_world(name.c_str(), g_opt.height)) return 1;
    if (!g_opt.haveAt) { g_opt.at[0] = 65536.5f; g_opt.at[1] = 60.0f; g_opt.at[2] = 65536.5f; }
    eden_console_teleport(g_opt.at[0], g_opt.at[1], g_opt.at[2]);
    tick(g_opt.frames);
    Console::init();
    Console::run("day");
    const Site site = build_site(player_state());
    std::printf("[eden-scene] site x0=%d z0=%d ground=%d height=%d\n", site.x0, site.z0, site.g, g_world_height);
    hold_pose(site, kPoses[0], 240);   // meshing, sign textures, liquids settle

    int written = 0;
    for (int night = 0; night < 2; ++night) {
        const char* tod = night ? "night" : "day";
        Console::run(tod);
        hold_pose(site, kPoses[0], 120);
        eden_rng_seed(1);
        spawn_dynamic(site);
        std::printf("[eden-scene] %s models %s creatures_on=%d\n", tod, eden_debug_model_state(), (int)CREATURES_ON);
        for (int p = 0; p < 3; ++p) {
            hold_pose(site, kPoses[p], 30);
            char base[64];
            std::snprintf(base, sizeof base, "%s-p%d", tod, p);
            // The trace rides the full capture itself: an extra render would advance the particle
            // systems (some animate in render) and shift every later capture.
            FILE* tf = (!g_opt.stateTrace.empty() && !night && p == 0) ? std::fopen(g_opt.stateTrace.c_str(), "w") : nullptr;
            if (tf) eden_gl_trace_begin(tf);
            if (shot(std::string(base) + "-full", EDEN_RC_ALL)) ++written;
            if (tf) {
                eden_gl_trace_end();
                std::fclose(tf);
                std::printf("[eden-scene] state trace -> %s\n", g_opt.stateTrace.c_str());
            }
            for (unsigned bit : kPassBits)
                if (shot(std::string(base) + "-" + eden_render_category_name((int)bit), bit)) ++written;
        }
    }
    g_app->viewController.setFixedEtime(0.0f);
    eden_rng_unpin();
    g_pass.track = false;
    g_ignoreUserInput = false;
    std::printf("[eden-scene] %d captures, prefix %s\n", written, g_opt.shot.c_str());
    return written == 2 * 3 * 11 ? 0 : 1;
}

}  // namespace g_render
}  // namespace harness

namespace harness {
namespace g_render {

namespace {
// One invariant line from the counters (rcore::inv()).
void invariants_clean(const char* phase) {
    const rcore::Invariants& v = rcore::inv();
    char d[400];
    std::snprintf(d, sizeof d, "frames %ld draws %ld outside %ld nested %ld unmatched %ld stacks %ld 2d %ld scene %ld "
                  "passes %ld contracts %ld/%ld%s%s", v.frames, v.draws, v.drawsOutsideFrame, v.nestedFrames,
                  v.endWithoutBegin, v.stackUnbalancedFrames, v.unbalanced2D, v.unbalancedScene, v.passUnbalancedFrames,
                  v.contractFails, v.contractChecks, v.firstFailure.empty() ? "" : "; first: ", v.firstFailure.c_str());
    const bool ok = v.frames > 0 && v.draws > 0 && v.drawsOutsideFrame == 0 && v.nestedFrames == 0 &&
                    v.endWithoutBegin == 0 && v.stackUnbalancedFrames == 0 && v.unbalanced2D == 0 &&
                    v.unbalancedScene == 0 && v.passUnbalancedFrames == 0 && v.contractFails == 0;
    char what[80];
    std::snprintf(what, sizeof what, "invariants hold: %s", phase);
    check(ok, what, d);
}
}  // namespace

// --render-selftest (headless, CI) / --render-selftest-gl (a window: also the GL unit checks).
int run_render_selftest() {
    g_selftestTag = "eden-render";
    const bool gl = eden_gl_have_context() != 0;
    char d[200];

    // R2: the stacks (the ES 1.1 entry points that forwarded here went with the shim in G.5b-7).
    {
        const int d0 = R::mv().depth();
        R::mv().push();
        R::mv().translate(1.0f, 2.0f, 3.0f);
        const float* m = R::mv().top().m;
        const bool depth = R::mv().depth() == d0 + 1;
        const bool same = m[12] == 1.0f && m[13] == 2.0f && m[14] == 3.0f && m[0] == 1.0f && m[15] == 1.0f;
        R::mv().pop();
        std::snprintf(d, sizeof d, "depth %d -> %d -> %d", d0, d0 + 1, R::mv().depth());
        check(depth && same && R::mv().depth() == d0, "R2: R::mv() push/translate/pop", d);
    }

    // The null backend's ids (headless only: with a context R:: goes to GL).
    if (!gl) {
        const R::TexDesc td = {4, 4, R::TF_RGBA8, false, false, false, false, nullptr};
        // Since G.5b-3 Texture2D's textures are null-backend textures too, so count against what is live now.
        const long before = rnull::liveTextures() + rnull::liveBuffers();
        const R::Tex t1 = R::texCreate(td, nullptr), t2 = R::texCreate(td, nullptr);
        const R::Buf b1 = R::bufCreate(R::BK_VERTEX, R::U_STATIC), b2 = R::bufCreate(R::BK_INDEX, R::U_STATIC);
        std::snprintf(d, sizeof d, "tex %u %u buf %u %u", t1, t2, b1, b2);
        check(t1 && t2 && b1 && b2 && t1 != t2 && b1 != b2 && t1 != b1, "null backend: ids nonzero and distinct", d);
        const long live = rnull::liveTextures() + rnull::liveBuffers() - before;
        R::texDestroy(t1); R::texDestroy(t2); R::bufDestroy(b1); R::bufDestroy(b2);
        std::snprintf(d, sizeof d, "live +%ld -> +%ld", live, rnull::liveTextures() + rnull::liveBuffers() - before);
        check(live == 4 && rnull::liveTextures() + rnull::liveBuffers() == before, "null backend: destroy releases", d);
        const float v[6] = {0, 0, 1, 0, 0, 1};
        const R::Geometry g = {&R::kFmtUI2, 0, v, 0, nullptr, R::I_NONE, 0, 3, R::TRIANGLES};
        const long n0 = rnull::draws();
        R::beginFrame(); R::draw2D(g, 0, {1, 1, 1, 1}); R::endFrame();
        check(rnull::draws() == n0 + 1, "null backend: a draw is counted", "");
    }

    // Negative self-checks: each invariant must notice its own violation.
    {
        rcore::inv() = rcore::Invariants();
        const float v[6] = {0, 0, 1, 0, 0, 1};
        const R::Geometry g = {&R::kFmtUI2, 0, v, 0, nullptr, R::I_NONE, 0, 3, R::TRIANGLES};
        R::draw2D(g, 0, {1, 1, 1, 1});                                // outside a frame
        R::endFrame();                                                // without a begin
        R::beginFrame(); R::mv().push(); R::endFrame(); R::mv().pop(); // unbalanced stack
        R::beginFrame(); R::begin2D(10, 10, 1); R::endFrame(); R::end2D();   // 2D left open (+ end2D unmatched)
        R::beginFrame(); R::beginFrame(); R::endFrame();              // nested
        const rcore::Invariants& n = rcore::inv();
        std::snprintf(d, sizeof d, "outside %ld unmatched %ld stacks %ld 2d %ld nested %ld", n.drawsOutsideFrame,
                      n.endWithoutBegin, n.stackUnbalancedFrames, n.unbalanced2D, n.nestedFrames);
        // The 2D frame left its ortho pushed, so it counts as a stack imbalance too (2).
        check(n.drawsOutsideFrame == 1 && n.endWithoutBegin == 1 && n.stackUnbalancedFrames == 2 &&
              n.unbalanced2D == 2 && n.nestedFrames == 1, "negative checks: every invariant fires", d);
        rcore::inv() = rcore::Invariants();
    }

    // The real thing: the menu, a world in play, its HUD menu and the console, with the pass tracker and
    // the signs pass contract on.
    g_pass.track = true;
    eden_render_pass_reset();
    rcore::setPassContracts(true);
    rcore::inv() = rcore::Invariants();
    tick(30);
    invariants_clean("menu");
    const char* name = g_opt.world.empty() ? "render-selftest" : g_opt.world.c_str();
    if (!open_world(name, g_opt.height)) { rcore::setPassContracts(false); g_pass.track = false; return 1; }
    tick(g_opt.frames);
    invariants_clean("menu -> world in play");
    const long checks = rcore::inv().contractChecks;
    std::snprintf(d, sizeof d, "%ld checks", checks);
    check(checks > 0, "the signs pass contract was checked in play", d);
    ensure_hud_menu_open();
    tick(20);
    eden_console_menu_open();
    tick(20);
    eden_console_menu_back();
    tick(20);
    invariants_clean("pause menu + console");
    quit_to_menu();
    tick(30);
    invariants_clean("back at the menu");
    rcore::setPassContracts(false);
    g_pass.track = false;

    if (gl) {
        std::string log;
        const int fails = rgl::selftest(&log);
        std::snprintf(d, sizeof d, "%d failed", fails);
        check(fails == 0, "GL unit checks (programs vs CPU, lit vs CPU, R3, R8)", d);
    }
    std::printf("[eden-render] %s: %d failure(s)\n", gl ? "--render-selftest-gl" : "--render-selftest",
                g_selftestFailures);
    return g_selftestFailures ? 1 : 0;
}
// --lit-oracle (G.5b-4, macOS: a window): the lit program against real fixed-function GL (renderer plan
// §7). Seven scenes (doors day/night, the firework cube, flat-shaded debris under its far light, the
// golden cube day/night with specular, a directional + second light); max delta <= 2/255 inside each.
int run_lit_oracle() {
    if (!eden_gl_have_context()) {
        std::printf("[lit-oracle] needs a window (a GL context)\n");
        return 2;
    }
    const int fails = rgl::litOracle(g_opt.litOracleDir.c_str());
    if (fails < 0) { std::printf("[lit-oracle] unavailable here (no legacy GL context)\n"); return 2; }
    std::printf("[lit-oracle] %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
}  // namespace g_render
}  // namespace harness
