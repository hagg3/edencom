// PassTimers.h -- P.1b per-pass frame instruments (ROADMAP Stage P, WORKING/low-end-perf-audit-2026-10-12.md).
// Where a frame goes: CPU milliseconds per update/render pass, plus the GL shim's draw / vertex /
// setup-call counters differenced around each pass. Compiled in only under EDEN_DIAGNOSTICS (every
// CI build, including the device ipa); otherwise every hook is an empty inline. With diagnostics on
// but the timers off (the default -- only a harness turns them on) a hook is one branch on a bool.
// With g_pass.finish set, each pass boundary calls glFinish first, so a pass's time is an UPPER
// BOUND that includes its GPU work; the frame it measures is no longer a normal frame.
// Passes nest (P_R_OPAQUE contains P_R_CULL, P_R_CHUNKS, P_R_OBJECTS, P_R_SKY): a parent's time
// includes its children's. Usage: native/README.md "--pass-probe"; doc: docs/rendering.md
// "Per-pass instruments". Header-only (C++17 inline variable), like F3Timing.h.
#ifndef PASS_TIMERS_H
#define PASS_TIMERS_H

enum EdenPass {
    P_UPDATE,        // World::update, whole
    P_U_TERRAIN,     //   terrain->update: streaming, dynamics, liquids, fire
    P_U_HUD,         //   hud->update: input, and the player's move through it
    P_U_CREATURES,   //   UpdateModels
    P_U_PLAYER,      //   player->preupdate
    P_U_EFFECTS,     //   effects->update + CmdScript::update
    P_U_MESH,        //   terrain->prepareAndLoadGeometry: meshing
    P_U_UPLOAD,      //   terrain->updateAllImportantChunks: VBO uploads
    P_RENDER,        // World::renderFrame's play branch, whole (not the menu)
    P_R_SETUP,       //   scene-pass begin + prepareScene + cam->render
    P_R_OPAQUE,      //   Terrain::render, whole
    P_R_CULL,        //     renderTree (both passes) + the transparent back-to-front qsort
    P_R_CHUNKS,      //     the opaque chunk draw loop
    P_R_OBJECTS,     //     immediate batches: doors, golden cubes, portals, swirls (+ flowers, in render2)
    P_R_SKY,         //     the sky box
    P_R_MODELS,      //   RenderModels (creatures)
    P_R_SIGNS,       //   SignRenderer::render
    P_R_TRANSPARENT, //   Terrain::render2, whole
    P_R_TCHUNKS,     //     the transparent chunk draw loop
    P_R_EFFECTS,     //   effects + fireworks + player->render
    P_R_UPSCALE,     //   eden_scene_pass_end: the render-scale blit
    P_R_HUD,         //   hud + in-game settings (not a GL dialog, which draws after the play branch)
    P_PRESENT,       // the native seam's present/swap -- with vsync off, time blocked here is the GPU proxy
    P_COUNT
};


extern "C" double eden_platform_now_ms(void);
extern "C" long eden_gl_stat_live(int which);
extern "C" void eden_gl_finish(void);
// G.5b-0: the renderer's pass tracker (port/src/render/render_core.cpp), for --scene-shot's per-pass
// draw mask, --state-trace's pass column and --render-selftest's pass contracts. Called only while a
// harness has set g_pass.track.
extern "C" void eden_render_pass_mark(int pass, int begin);

static const char* const kEdenPassNames[P_COUNT] = {
    "update", "u_terrain", "u_hud", "u_creatures", "u_player", "u_effects", "u_mesh", "u_upload",
    "render", "r_setup", "r_opaque", "r_cull", "r_chunks", "r_objects", "r_sky", "r_models",
    "r_signs", "r_transparent", "r_tchunks", "r_effects", "r_upscale", "r_hud", "present",
};

struct PassState {
    bool on = false;       // set by a harness (--pass-probe, --pass-timers)
    bool finish = false;   // glFinish at every pass boundary (--pass-finish)
    bool track = false;    // G.5b-0: tell the renderer which pass each draw belongs to (--scene-shot, --state-trace, --render-selftest)
    // per frame, cleared by pass_frame_reset(); a pass entered twice in a frame accumulates
    double ms[P_COUNT] = {0};
    long draws[P_COUNT] = {0}, verts[P_COUNT] = {0}, setup[P_COUNT] = {0};
    // open-pass snapshots
    double t0[P_COUNT] = {0};
    long d0[P_COUNT] = {0}, v0[P_COUNT] = {0}, s0[P_COUNT] = {0};
};
inline PassState g_pass;

static inline void pass_frame_reset() {
    for (int i = 0; i < P_COUNT; ++i) { g_pass.ms[i] = 0; g_pass.draws[i] = g_pass.verts[i] = g_pass.setup[i] = 0; }
}
static inline void pass_begin(int p) {
    if (g_pass.track) eden_render_pass_mark(p, 1);
    if (!g_pass.on) return;
    if (g_pass.finish) eden_gl_finish();
    g_pass.d0[p] = eden_gl_stat_live(0); g_pass.v0[p] = eden_gl_stat_live(4); g_pass.s0[p] = eden_gl_stat_live(1);
    g_pass.t0[p] = eden_platform_now_ms();
}
static inline void pass_end(int p) {
    if (g_pass.track) eden_render_pass_mark(p, 0);
    if (!g_pass.on) return;
    if (g_pass.finish) eden_gl_finish();
    g_pass.ms[p] += eden_platform_now_ms() - g_pass.t0[p];
    g_pass.draws[p] += eden_gl_stat_live(0) - g_pass.d0[p];
    g_pass.verts[p] += eden_gl_stat_live(4) - g_pass.v0[p];
    g_pass.setup[p] += eden_gl_stat_live(1) - g_pass.s0[p];
}


// Scope form, for a pass that has more than one exit.
struct PassScope {
    int p;
    explicit PassScope(int pass) : p(pass) { pass_begin(p); }
    ~PassScope() { pass_end(p); }
};

#endif
