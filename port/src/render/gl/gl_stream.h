// gl_stream.h — the two client-memory streaming rings (Q.5), shared by the shim's client-array draws
// and the renderer's vmem/imem draws (G.5b-0: lifted out of gl_fixed_function.cpp unchanged).
//
// Each upload takes the next free range: glMapBufferRange(UNSYNCHRONIZED | INVALIDATE_RANGE), write,
// unmap. The ring is orphaned (glBufferData(NULL)) only when a range doesn't fit, so no range is ever
// written twice inside one storage generation and the GPU may still read earlier ranges without a sync.
// `size` grows to the largest single upload seen. The caller binds r.target to r.name first.
#ifndef EDEN_RENDER_GL_STREAM_H
#define EDEN_RENDER_GL_STREAM_H

#include "gl_state.h"

namespace rgl {

struct StreamRing {
    GLuint     name = 0;
    GLenum     target;
    GLsizeiptr size;
    GLsizeiptr head = 0;
    bool       fresh = true;   // no storage yet: the first allocation must glBufferData
};
extern StreamRing g_vring;     // 4 MB, GL_ARRAY_BUFFER
extern StreamRing g_iring;     // 256 KB, GL_ELEMENT_ARRAY_BUFFER

void ensureRings();            // generate the two names (a context is live)
// Reserve `bytes` at a 16-byte-aligned offset plus `phase` (0..3); returns a pointer to write through,
// *off is the GL offset. streamEnd publishes. A refused map stages in memory (glBufferSubData).
void* streamBegin(StreamRing& r, GLsizeiptr bytes, GLsizeiptr phase, GLintptr* off);
void  streamEnd(StreamRing& r, GLintptr off, GLsizeiptr bytes);
void  invalidateRings();

GLsizei typeSize(GLenum type);

}  // namespace rgl

#endif  // EDEN_RENDER_GL_STREAM_H
