// gl_stream.cpp — see gl_stream.h. Moved verbatim from gl_fixed_function.cpp (G.5b-0).
#include "gl_stream.h"

#include <vector>

namespace rgl {

StreamRing g_vring = {0, GL_ARRAY_BUFFER, (GLsizeiptr)4 << 20};
StreamRing g_iring = {0, GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)256 << 10};

static std::vector<unsigned char> g_stream_scratch;
static bool g_stream_mapped = false;

void ensureRings() {
    if (!g_vring.name) glGenBuffers(1, &g_vring.name);
    if (!g_iring.name) glGenBuffers(1, &g_iring.name);
}

void* streamBegin(StreamRing& r, GLsizeiptr bytes, GLsizeiptr phase, GLintptr* off) {
    GLsizeiptr at = ((r.head + 15) & ~(GLsizeiptr)15) + phase;
    if (r.fresh || at + bytes > r.size) {
        if (bytes + phase > r.size) r.size = (bytes + phase + 0xFFFF) & ~(GLsizeiptr)0xFFFF;
        glBufferData(r.target, r.size, nullptr, GL_STREAM_DRAW);   // orphan: a new generation
        stat_issued();
        r.fresh = false;
        at = phase;
    }
    r.head = at + bytes;
    *off = (GLintptr)at;
    void* p = glMapBufferRange(r.target, (GLintptr)at, bytes,
                               GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT |
                               GL_MAP_UNSYNCHRONIZED_BIT);
    stat_issued();
    g_stream_mapped = (p != nullptr);
    if (p) return p;
    if (g_stream_scratch.size() < (size_t)bytes) g_stream_scratch.resize((size_t)bytes);
    return g_stream_scratch.data();
}

void streamEnd(StreamRing& r, GLintptr off, GLsizeiptr bytes) {
    if (g_stream_mapped) glUnmapBuffer(r.target);
    else glBufferSubData(r.target, off, bytes, g_stream_scratch.data());
    stat_issued();
}

void invalidateRings() {
    g_vring.name = 0; g_vring.head = 0; g_vring.fresh = true;
    g_iring.name = 0; g_iring.head = 0; g_iring.fresh = true;
}

GLsizei typeSize(GLenum type) {
    switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE:   return 1;
        case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
        case GL_FIXED: case GL_FLOAT:          return 4;
        default:                               return 4;
    }
}

}  // namespace rgl
