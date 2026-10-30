// gl_context.h — the GL context, the drawable and the GL instruments, as the host and the harness see
// them (G.5b-7). Plain C: no GL types, no GL header, so including it gives a host file no way to call GL.
// Until G.5b-7 these were the context group of the ES 1.1 shim's header (gl_es1_shim.h, deleted with
// the shim; its history is in git and port/docs/history/gl-shim.md). Defined in gl_context_native.cpp (context,
// drawable, pick viewport, scene pass), gl_state.cpp (the counters) and render_core.cpp (the trace).
// Main thread only (root CLAUDE.md rule 8).
#ifndef EDEN_RENDER_GL_CONTEXT_H
#define EDEN_RENDER_GL_CONTEXT_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// 1 while a real GL context is current; 0 headless (or before creation / after a failed one), when
// every R:: call goes to the null backend.
int  eden_gl_have_context(void);
int  eden_gl_context_is_lost(void);   // always 0 natively (no context loss on SDL's GL)

// Context ownership. The context exists before the engine is built: Graphics::initGraphics runs during
// World::World(). Returns 1 on success, 0 headless (not an error).
int  eden_gl_context_create(int drawable_width, int drawable_height);
// PW.4 probe: --drawable=1x. Call before eden_gl_context_create: the window is made without
// HIGH_PIXEL_DENSITY, so the drawable is point-sized (1024x768 on an Air 2), not Retina.
void eden_gl_context_set_drawable_1x(int on);
void eden_gl_context_destroy(void);
// Drawable size in PIXELS. Not the pick viewport (below).
void eden_gl_context_get_drawable_size(int* width, int* height);
// The size R::pickViewport answers: SCREEN_WIDTH*SCALE_WIDTH x SCREEN_HEIGHT*SCALE_HEIGHT, the engine's
// POINT space, never the real drawable (picking's gluUnProject assumes it). DisplayProfile.cpp calls it
// whenever the point space changes; a non-positive size is ignored.
void eden_gl_set_pick_viewport(int width, int height);
void eden_gl_context_set_drawable_size(int width, int height);
void eden_set_drawable_size(int width, int height);
// Binds the default framebuffer and the letterbox viewport.
void eden_gl_context_bind_default_framebuffer(void);
// The 3D pass's render resolution (N.4.11): below 100 % render scale the play-mode world pass draws into
// an offscreen framebuffer that _end upscales into the letterbox; at 100 % both are no-ops.
void eden_scene_pass_begin(void);
void eden_scene_pass_end(void);

// Draw-path accounting (audit item #4, P.1b): 0=draws, 1=setup calls issued, 2=elided by the caches,
// 3=issued+elided, 4=vertices. eden_gl_stat: the last completed frame; _live: the frame in progress.
int  eden_gl_stat(int which);
long eden_gl_stat_live(int which);
void eden_gl_finish(void);              // glFinish when a context is live (P.1b's opt-in per-pass mode)
void eden_gl_stats_frame_boundary(void);   // once per frame, before the engine renders

// --state-trace (G.5b-0): one line per R:: draw while a file is open (render_core.cpp).
void eden_gl_trace_begin(FILE* f);
void eden_gl_trace_end(void);

#ifdef __cplusplus
}
#endif

#endif  // EDEN_RENDER_GL_CONTEXT_H
