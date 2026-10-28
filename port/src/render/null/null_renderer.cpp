// null_renderer.cpp — the renderer with no context (headless runs, a failed context; plan §5). Resource
// ids are nonzero and distinct (the engine tests handles against 0, like the shim's fake GL names) and
// draws are counted, so --render-selftest can assert the frame structure and the invariants without GL.
#include "../render_internal.h"

#include <unordered_set>

namespace rnull {

namespace {
uint32_t g_next = 0x40000000u;   // apart from the shim's fake GL names, which count up from 1
std::unordered_set<uint32_t> g_tex, g_buf;
long g_draws = 0;
}  // namespace

R::Tex texCreate(const R::TexDesc&) { uint32_t t = ++g_next; g_tex.insert(t); return t; }
void texDestroy(R::Tex t) { g_tex.erase(t); }
R::Buf bufCreate() { uint32_t b = ++g_next; g_buf.insert(b); return b; }
void bufDestroy(R::Buf b) { g_buf.erase(b); }
void draw(const R::Geometry& g) { if (g.fmt && g.count > 0) ++g_draws; }
long draws() { return g_draws; }
long liveTextures() { return (long)g_tex.size(); }
long liveBuffers() { return (long)g_buf.size(); }

}  // namespace rnull
