// gl_es1_extras_native.cpp — the two ES 1.1 entry points that only the vendored PVRT SDK calls
// (Phase N Stage 1, WORKING/native-migration-plan-2026-09-04.md).
//
// WHY THEY ARE HERE AND NOT IN gl_fixed_function.cpp: the shim's inventory in gl_es1_shim.h was
// built by grepping the ENGINE (Classes/*.mm + Lighting.mm) for GL calls, deliberately excluding
// the PVRT SDK's own — so neither of these is in it. On web that is invisible, because emsdk's
// real <GLES/gl.h> declares them and emscripten's GL library defines them. On native the ES1
// header is this port's own enum-only one, so the two calls in Classes/PVRTPrint3DAPI.cpp
// (glClientActiveTexture at :431/:564/:574, glTexEnvf at :439 via the myglTexEnv macro) have
// nowhere to resolve.
//
// Keeping them in a native-only file rather than growing gl_es1_shim.h's inventory is the honest
// split: that header's contents are "what the engine calls, and what the shim therefore promises
// to translate". These two are neither — they are what a vendored SDK's debug-text renderer calls.
//
// PVRTPrint3D is the PowerVR SDK's on-screen debug text. Eden does not use it (docs/ui.md: the
// game's UI is entirely its own GL drawing), but PVRTPrint3DAPI.cpp is in the authoritative
// Eden.xcodeproj source list and so is compiled and linked. These definitions exist so it LINKS;
// if it ever actually ran, see the per-function notes for what it would and would not get.

#define EDEN_GL_NO_GUARD 1
#include "gl_es1_shim.h"

extern "C" {

// ES1 multitexture: selects which texture unit subsequent glTexCoordPointer /
// glEnableClientState(GL_TEXTURE_COORD_ARRAY) calls apply to. This shim models a SINGLE texture
// unit (one `u_tex` sampler, one ATTR_TEXCOORD slot — gl_fixed_function.cpp GROUP 2), which is
// all the engine's own draw path ever uses. Selecting unit 0 is therefore already the state; any
// other unit is a request the shim cannot honour, and silently ignoring it is better than the
// alternative here (aborting inside a debug-text renderer that is not on any live path).
void glClientActiveTexture(GLenum texture) {
    (void)texture;
}

// The float spelling of glTexEnvi, which the shim does implement (it is how the engine selects
// GL_MODULATE vs GL_DECAL — see gl_fixed_function.cpp's u_texEnvDecal uniform). Every ES1
// texture-env parameter the SDK passes is an enum in float clothing, so the cast is exact rather
// than lossy: GL_MODULATE is 0x2100, comfortably inside float's integer-exact range.
void glTexEnvf(GLenum target, GLenum pname, GLfloat param) {
    glTexEnvi(target, pname, (GLint)param);
}

}  // extern "C"
