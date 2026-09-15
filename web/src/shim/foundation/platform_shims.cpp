// platform_shims.cpp — see platform_shims.h for what belongs here and why.
#include "platform_shims.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#include <chrono>
#endif

extern "C" double eden_platform_now_ms(void) {
#if defined(__EMSCRIPTEN__)
  return emscripten_get_now();
#else
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
#endif
}

// ---------------------------------------------------------------------------------------------
// Filesystem roots (Phase N Stage 1). See platform_shims.h for what each one means.
//
// The defaults are the literals the Foundation shim used to carry inline, so the web target's
// behaviour is unchanged to the byte. A native entry point overrides them once at startup with
// real paths (macOS: ~/Library/Application Support/Emod and the repo's asset tree).
//
// std::string rather than a raw pointer because the caller's argv strings outlive the process
// anyway but a --docs=... value assembled at runtime would not; owning a copy removes the
// question entirely.
// ---------------------------------------------------------------------------------------------
#include <string>

namespace {
std::string& documents_root() {
  static std::string root = "/documents";
  return root;
}
std::string& bundle_root() {
  static std::string root = "/bundle";
  return root;
}
}  // namespace

extern "C" const char* eden_platform_documents_root(void) { return documents_root().c_str(); }
extern "C" const char* eden_platform_bundle_root(void) { return bundle_root().c_str(); }

extern "C" void eden_platform_set_roots(const char* documents, const char* bundle) {
  if (documents && *documents) {
    documents_root() = documents;
    // A trailing slash would turn every "<root>/<name>" into "<root>//<name>". Harmless on POSIX,
    // but the engine also COMPARES these paths (FileManager's world list), so normalise.
    while (documents_root().size() > 1 && documents_root().back() == '/') documents_root().pop_back();
  }
  if (bundle && *bundle) {
    bundle_root() = bundle;
    while (bundle_root().size() > 1 && bundle_root().back() == '/') bundle_root().pop_back();
  }
}

namespace {

// xoshiro128** — small, fast, and good enough that `arc4random() % 200 - 100` (BlockBreak.mm's
// particle scatter) looks right. NOT cryptographic, unlike the real arc4random; that difference
// is safe here because every call site in this tree is gameplay dice. If a later stage ever needs
// randomness for something security-relevant, use crypto.getRandomValues via EM_ASM instead of
// widening this.
struct State {
  uint32_t s[4];

  State() {
    // Seed from the page's high-resolution clock. Deterministic seeding was considered and
    // rejected: the engine's own worldgen is offline (docs/terrain-generation.md — the shipped
    // Eden.eden is pre-generated), so nothing here needs reproducibility, and identical particle
    // scatter on every page load would be visible.
    uint64_t seed = (uint64_t)(eden_platform_now_ms() * 1000.0);
    // SplitMix64 to spread the low-entropy clock value across all four words.
    for (int i = 0; i < 4; i++) {
      seed += 0x9E3779B97F4A7C15ULL;
      uint64_t z = seed;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
      s[i] = (uint32_t)((z ^ (z >> 31)) >> 32);
    }
    if (!(s[0] | s[1] | s[2] | s[3])) s[0] = 1;  // all-zero state is a fixed point
  }
};

State &state() {
  static State g;
  return g;
}

inline uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

}  // namespace

extern "C" {

// Compiled only where the platform's libc does not already provide these — see the block in
// platform_shims.h. On glibc 2.36+, Apple and musl 1.2.3+ this whole section is absent and the
// engine calls the real thing.
#if defined(EDEN_PROVIDE_ARC4RANDOM)
uint32_t arc4random(void) {
  State &g = state();
  const uint32_t result = rotl(g.s[1] * 5, 7) * 9;
  const uint32_t t = g.s[1] << 9;
  g.s[2] ^= g.s[0];
  g.s[3] ^= g.s[1];
  g.s[1] ^= g.s[2];
  g.s[0] ^= g.s[3];
  g.s[2] ^= t;
  g.s[3] = rotl(g.s[3], 11);
  return result;
}

uint32_t arc4random_uniform(uint32_t upper_bound) {
  if (upper_bound < 2) return 0;
  // Rejection sampling, matching BSD's — avoids the modulo bias that plain `arc4random() % n`
  // has. (The engine's own call sites all use plain `%`; this function exists for completeness
  // and for any new port-side code, which should prefer it.)
  const uint32_t min = (uint32_t)(-upper_bound) % upper_bound;
  uint32_t r;
  do {
    r = arc4random();
  } while (r < min);
  return r % upper_bound;
}
#endif  // EDEN_PROVIDE_ARC4RANDOM

#if defined(EDEN_PROVIDE_RANDOM)
// Windows only — see platform_shims.h. Built on the same generator as arc4random above (which is
// compiled on this target too), so there is one RNG in this file rather than two. The shift makes
// it non-negative, which is the whole of BSD random()'s contract that any caller here relies on.
long random(void) { return (long)(arc4random() >> 1); }

// The generator seeds itself from the clock on first use, so an explicit seed only matters for
// reproducibility. Honoured rather than ignored: worldgen is exactly the kind of caller that would
// want it, even though this port's worldgen runs offline.
void srandom(unsigned seed) {
  State &g = state();
  g.s[0] = seed ? seed : 1u;
  g.s[1] = g.s[0] ^ 0x9E3779B9u;
  g.s[2] = g.s[1] ^ 0x85EBCA6Bu;
  g.s[3] = g.s[2] ^ 0xC2B2AE35u;
}
#endif  // EDEN_PROVIDE_RANDOM

}  // extern "C"
