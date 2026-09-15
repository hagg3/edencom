// <GLES2/gl2.h> for the NATIVE target — Phase N Stage 1.
//
// gl_fixed_function.cpp includes this to reach the programmable-pipeline entry points (shaders,
// uniforms, vertex attributes, VBOs) that the ES1 header does not declare. On desktop GL those
// live in the SAME header as everything else, so this is a one-line alias — kept as a real file
// rather than an -I trick so the include in gl_fixed_function.cpp needs no #ifdef.
#ifndef EDEN_NATIVE_GLES2_GL2_H
#define EDEN_NATIVE_GLES2_GL2_H
#include "../GLES/gl.h"
#endif
