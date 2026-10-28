// <GLES2/gl2.h> for the NATIVE target — Phase N Stage 1.
//
// The GL backend (gl_state.h) includes this for the programmable-pipeline entry points (shaders,
// uniforms, vertex attributes, VBOs). On every target those live in the SAME header as everything
// else (framework/GLES/gl.h), so this is a one-line alias kept so the include needs no #ifdef.
#ifndef EDEN_NATIVE_GLES2_GL2_H
#define EDEN_NATIVE_GLES2_GL2_H
#include "../GLES/gl.h"
#endif
