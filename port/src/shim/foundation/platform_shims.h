// platform_shims.h — small platform primitives the engine gets from iOS's libc/Foundation but
// that Emscripten's sysroot does not provide. Included from the <Foundation/Foundation.h>
// trampoline, because that is how engine files receive these on device (via the prefix header)
// and this port does not edit engine sources.
//
// Two things live here, both named explicitly by web-port-plan.md's Stage P1 list of seams
// ("Replace arc4random→arc4random-equivalent, CFAbsoluteTime→…"):
//
//   * arc4random  — BSD libc on Darwin, absent from Emscripten's libc entirely (checked: it
//                   appears nowhere in the sysroot except libc++ internals). 17 call sites,
//                   all gameplay randomness (BlockBreak particle scatter, creature AI).
//   * MAX / MIN   — Foundation's <NSObjCRuntime.h> macros, used by BlockBreak.mm and Model.mm.
//
// (CFAbsoluteTimeGetCurrent, the third Stage P1 seam, is in framework/CoreFoundation/ instead,
// since it is a genuine CF function rather than a libc gap.)
#ifndef EDEN_SHIM_PLATFORM_SHIMS_H
#define EDEN_SHIM_PLATFORM_SHIMS_H

#include <stdint.h>

// (EDEN_BUILD_HAS_THREADS went in G.3 with its three users, the shim ObjC runtime's dispatch cache,
// NSAutoreleasePool.mm and NSObject.mm: every native build has real threads.)

#ifdef __cplusplus
extern "C" {
#endif

// Same contract as BSD's: a uniformly distributed uint32_t, no seeding required, never fails.
// The implementation is NOT cryptographic (see platform_shims.cpp) — every call site in this
// engine is gameplay dice, and nothing here guards a secret.
//
// *** DECLARED ONLY WHERE THE PLATFORM'S LIBC DOES NOT ALREADY HAVE IT. *** Phase N Stage 3.2,
// found by the Linux leg: **glibc has shipped arc4random and arc4random_uniform since 2.36**
// (2022), declared `__THROW`. Redeclaring them here without `noexcept` is not a duplicate
// declaration that the compiler tolerates — it is an exception-specification MISMATCH, and it
// fails inside `<stdlib.h>` in every translation unit that reaches <string>, naming glibc's file
// and pointing back here as "previous declaration". This header's opening premise, that
// arc4random is what iOS provides and Emscripten does not, was true of the only two targets that
// existed when it was written.
//
// The set that genuinely lacks it is Emscripten and mingw. Apple has it (it is BSD libc), glibc
// 2.36+ has it, musl 1.2.3+ has it. Where the platform provides it, <stdlib.h> — already included
// by the Foundation trampoline for every engine file — is the declaration, and
// platform_shims.cpp compiles no definition.
#if defined(_WIN32) || \
    (defined(__GLIBC__) && !(__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 36)))
#define EDEN_PROVIDE_ARC4RANDOM 1
uint32_t arc4random(void);
uint32_t arc4random_uniform(uint32_t upper_bound);
#endif

// BSD's `random()`/`srandom()`, which Classes/TerrainGenerator.mm calls directly (four sites) and
// which the mingw CRT does not have — it ships `rand()` only. Same shape as the block above and
// found the same way, by a Windows build failing on it. glibc, musl and Apple all provide them, so
// this is Windows-only; adding it unconditionally would be an exception-specification clash with
// glibc exactly like the arc4random one was.
//
// Contract as BSD's: a non-negative long in [0, 2^31-1]. NOT cryptographic — every caller here is
// worldgen dice, and worldgen is offline (see the root CLAUDE.md's "Change worldgen" workflow).
#if defined(_WIN32)
#define EDEN_PROVIDE_RANDOM 1
long random(void);
void srandom(unsigned seed);
#endif

// Phase N Stage 0.3 (WORKING/native-migration-plan-2026-09-04.md): a monotonic milliseconds
// clock, portable between Emscripten (emscripten_get_now(), i.e. performance.now()) and native
// (std::chrono::steady_clock). The four real emscripten_get_now() call sites in this port —
// this file's own RNG seed, NSFileHandle.mm's opt-in I/O timing, CoreFoundation.mm's
// CFAbsoluteTimeGetCurrent, and MeshTiming.cpp's mesh/upload timers — all only need
// monotonicity, never wall time or sub-call overhead, so one implementation covers all four.
double eden_platform_now_ms(void);

// Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md): the two filesystem roots the
// Foundation shim answers with. They were string literals — "/documents" and "/bundle" — which is
// exactly right for Emscripten, where the virtual FS root is ours to lay out and the CMake
// --preload-file rules put the assets at /bundle. A native process cannot write to /documents,
// so the shim asks for them instead of hard-coding them, and the values stay byte-identical on
// the web target (the defaults ARE the old literals).
//
// `documents` is the engine's save directory: NSSearchPathForDirectoriesInDomains(NSDocumentDirectory)
// answers with it, and FileManager.cpp builds every world path under it.
// `bundle` is the read-only asset root: NSBundle's -bundlePath/-resourcePath, and the base of the
// basename index NSBundle.mm builds over <bundle>/media.
//
// Both are absolute, without a trailing slash, and stay valid for the process lifetime.
const char* eden_platform_documents_root(void);
const char* eden_platform_bundle_root(void);
// Native entry points call this before anything touches the filesystem (i.e. before
// App::boot()). Passing NULL for either leaves that root at its default. No-op after the
// first filesystem access, so it is a startup switch, not a runtime one.
void eden_platform_set_roots(const char* documents, const char* bundle);

// G.5b-0: the engine's one RNG hook (eden_prefix.h renames every engine arc4random() call to
// eden_arc4random()). Unpinned it forwards to the platform's arc4random; eden_rng_seed() pins it to a
// PCG32 stream so a harness (--scene-shot) can replay particles, fire, creature spawns and wander
// exactly. Main thread only while pinned: the world-load thread must not draw from it.
uint32_t eden_arc4random(void);
void eden_rng_seed(uint64_t seed);   // pin (and restart) the stream
void eden_rng_unpin(void);
int  eden_rng_pinned(void);

#ifdef __cplusplus
}
#endif

// Phase N Stage 3: 64-BIT FILE OFFSETS, on a target where `long` is 32 bits.
// This port's save files are the reason. A 256z world runs to gigabytes (docs/save-load.md), the
// loader reads the ColumnIndex directory by seeking to a stored offset near the end, and the
// in-place save strategy seeks per column — so every offset in NSFileHandle.mm and
// save_backup.cpp is a real file position, not a small number.
//
// `fseeko`/`ftello` DO exist in the mingw CRT, which is what makes this quiet rather than a build
// error: their `off_t` is `long`, i.e. 32 bits on LLP64, so they simply stop working above 2 GB.
// The MS CRT's own 64-bit pair is _fseeki64/_ftelli64. Everywhere else the POSIX pair is already
// 64-bit (Emscripten, macOS and Linux all have a 64-bit off_t here) and these are it verbatim.
//
// Signed, deliberately: SEEK_CUR takes negative deltas and ftell reports -1 on error.
#if defined(_WIN32)
#define eden_fseek64(fp, off, whence) _fseeki64((fp), (long long)(off), (whence))
#define eden_ftell64(fp)              ((long long)_ftelli64(fp))
#else
#define eden_fseek64(fp, off, whence) fseeko((fp), (off_t)(off), (whence))
#define eden_ftell64(fp)              ((long long)ftello(fp))
#endif

// ...and the same shape for mkdir, for a smaller reason: the mingw CRT's takes ONE argument where
// POSIX's takes two, which is a compile error rather than a portability footnote.
// (port/src/seam/Prefs_native.cpp solves it with SDL_CreateDirectory, which it can: nothing under
// port/src/shim/foundation depends on SDL.)
#if defined(_WIN32)
#define eden_mkdir(path) _mkdir(path)
#else
#define eden_mkdir(path) mkdir((path), 0755)
#endif

// Foundation's definitions, guarded the same way Apple's are — several third-party files under
// Classes/ define these themselves before use.
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef ABS
#define ABS(a) (((a) < 0) ? (-(a)) : (a))
#endif

#endif
