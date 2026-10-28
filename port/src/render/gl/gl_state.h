// gl_state.h — the GL backend's state cache (G.5b-0; plan §5, R3; lifted out of the ES 1.1 shim, which
// G.5b-7 deleted): the draw-call accounting, the program / texture unit / vertex-attribute /
// buffer-binding caches with the glDeleteBuffers rule, the M0 buffer-byte accounting, and ACTUAL, the
// shadow of the texture binding, blend, depth, cull and polygon offset GL holds, so a draw issues only
// the deltas of the state it states (applyRenderer). Until G.5b-7 the shim's draws applied the ES 1.1
// code's LOGICAL state through the same shadow (the R3 bridge); the engine's 2D state is the pen now
// (Classes/Graphics.h), which pen draws pass as their R::State.
//
// Main thread only, like all GL (root CLAUDE.md rule 8). Every file including this one sees real GL.
#ifndef EDEN_RENDER_GL_STATE_H
#define EDEN_RENDER_GL_STATE_H

#include <GLES/gl.h>     // the platform's GL (framework/GLES/gl.h): types, entry points, ES 1.1-only enums
#include <GLES2/gl2.h>
#include "../../../../Classes/Renderer.h"

#include <cstddef>
#include <string>

namespace rgl {

// ---- draw-call accounting (audit item #4, P.1b) — eden_gl_stat()/eden_gl_stat_live() read it ------
struct DrawStats { long draws = 0, issued = 0, elided = 0, verts = 0; };
DrawStats& stats();              // the frame being drawn
const DrawStats& statsLast();    // the last completed frame
inline void stat_issued(long n = 1) { stats().issued += n; }
inline void stat_elided(long n = 1) { stats().elided += n; }

// ---- vertex attribute slots: every program binds these locations ----------------
enum AttrSlot { ATTR_POSITION = 0, ATTR_COLOR, ATTR_TEXCOORD, ATTR_NORMAL, ATTR_POINTSIZE,
                ATTR_MATRIXINDEX, ATTR_WEIGHT, ATTR_COUNT };
void bindAttribLocations(GLuint prog);

// What GL was last told per slot. Zero-initialised is the truthful state of a fresh context.
struct AttrCache {
    bool        enabled    = false;
    GLint       size       = 0;
    GLenum      type       = 0;
    GLboolean   normalized = GL_FALSE;
    GLsizei     stride     = 0;
    const void* pointer    = nullptr;
    GLuint      buffer     = 0;
};
extern AttrCache g_attrCache[ATTR_COUNT];
// Enable/specify one slot from a buffer (0 = the caller has bound nothing: the pointer is client
// memory and is invalid in core GL, never passed). Issues only the delta.
void attribPointer(int slot, GLuint buf, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* ptr);
void attribDisable(int slot);
// A disabled slot's constant value (renderer draws: the tint of a format with no colour, R5).
void attribConstant(int slot, float x, float y, float z, float w);

// ---- programs, texture unit ------------------------------------------------------------------------
void useProgram(GLuint prog);
void activeTexture0();

// ---- buffer bindings ---------------------------------------------------------------------------------
// The binding every draw hands back (restoreBindings). Until G.5b-7 these mirrored the engine's own
// glBindBuffer calls; no engine code binds a buffer since, so they are 0 unless a buffer is deleted.
// Kept so the path's bindings still have one resting value to return to.
extern GLuint g_bound_array_buffer;
extern GLuint g_bound_element_buffer;
extern GLuint g_path_array_buffer;   // what the draw path has bound right now
extern bool   g_path_ebo_dirty;      // the draw path moved GL_ELEMENT_ARRAY_BUFFER
void bindPathArray(GLuint buf);
void bindPathElements(GLuint buf);
void restoreBindings();
void forgetDeletedBuffers(GLsizei n, const GLuint* buffers);
// M0 (ROADMAP Phase M): resident buffer bytes, keyed by the binding `target` names right now.
void noteBufferDataFor(GLuint name, size_t size);
void noteBufferCreates(unsigned n);

// ---- ACTUAL: what GL holds ----------------------------------------------------------------------------
void bindTextureNow(GLuint t);       // a backend upload binds its own texture (ACTUAL follows)
void depthWriteNow(bool on);         // before glClear: a depth clear honours the depth mask
void applyRenderer(const R::State& s, GLuint tex);   // a draw: its own state, as deltas
void forgetTexture(GLuint name);     // glDeleteTextures: GL reverts a deleted binding to 0

// ---- lifetime ----------------------------------------------------------------------------------------
// The context is gone (or a new one is current): every cache describes objects that no longer exist.
void invalidate();

}  // namespace rgl

#endif  // EDEN_RENDER_GL_STATE_H
