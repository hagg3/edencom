// render_passes.h — G.5b-0: the pass tracker and per-pass draw mask (render_passes.cpp). Plain C so the
// GL shim, both renderer backends and the harness share it.
#ifndef EDEN_RENDER_PASSES_H
#define EDEN_RENDER_PASSES_H

// Draw categories (--scene-shot's per-pass captures). Bits, so a mask can hold several.
enum {
    EDEN_RC_CHUNKS      = 1 << 0,   // opaque chunk loop (P_R_CHUNKS)
    EDEN_RC_OBJECTS     = 1 << 1,   // doors, golden cubes, portals, swirls (P_R_OBJECTS in Terrain::render)
    EDEN_RC_SKY         = 1 << 2,   // P_R_SKY
    EDEN_RC_MODELS      = 1 << 3,   // creatures (P_R_MODELS)
    EDEN_RC_SIGNS       = 1 << 4,   // P_R_SIGNS
    EDEN_RC_TRANSPARENT = 1 << 5,   // transparent chunk loop (P_R_TCHUNKS)
    EDEN_RC_FLOWERS     = 1 << 6,   // P_R_OBJECTS inside Terrain::render2
    EDEN_RC_EFFECTS     = 1 << 7,   // effects, fireworks, player (P_R_EFFECTS)
    EDEN_RC_HUD         = 1 << 8,   // P_R_HUD
    EDEN_RC_OTHER       = 1 << 9,   // any draw outside the above (menus, dialogs, a pass's own frame)
    EDEN_RC_ALL         = (1 << 10) - 1
};

#ifdef __cplusplus
extern "C" {
#endif
void eden_render_pass_mark(int pass, int begin);   // Classes/PassTimers.h, while g_pass.track
int  eden_render_pass_depth(void);
int  eden_render_pass_innermost(void);             // -1 outside every pass
int  eden_render_pass_is_open(int pass);
int  eden_render_pass_unbalanced(void);            // ends that did not match (a --render-selftest invariant)
void eden_render_pass_reset(void);
const char* eden_render_pass_name(int pass);

int  eden_render_draw_category(void);              // one EDEN_RC_* bit for a draw issued now
const char* eden_render_category_name(int bit);
void eden_render_set_draw_mask(unsigned mask);     // EDEN_RC_ALL = draw everything (the default)
unsigned eden_render_draw_mask(void);
int  eden_render_mask_active(void);                // mask != ALL: glClear also clears colour, see the shim
int  eden_render_draw_allowed(void);
#ifdef __cplusplus
}
#endif

#endif  // EDEN_RENDER_PASSES_H
