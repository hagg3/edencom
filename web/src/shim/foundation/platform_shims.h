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

// Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md): portable stand-in for
// EMSCRIPTEN_KEEPALIVE on the seam files BOTH targets compile. It is the same call
// gl_es1_shim.h's EDEN_GL_EXPORT made in Stage 0.2, and Stage 0.4 explicitly deferred it to here:
// an export that exists so the host page can call it (Module._eden_*) is meaningless off
// Emscripten, but deleting the annotation would break web and forking the file would be worse.
//
// The macro's job on web is to keep the symbol alive through -flto and the linker's dead-symbol
// pass. On native every `extern "C"` symbol in the executable is already reachable from the
// binary's own code (the entry point calls them directly), so the no-op is correct rather than
// merely tolerable.
#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define EDEN_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EDEN_EXPORT
#endif

// --- "does this build have more than one thread?" --------------------------------------------
// Phase N Stage 3.2, and it cost a real bug. Three shim files carry per-thread state — the ObjC
// runtime's dispatch caches (objc_runtime.cpp), the autorelease-pool stack (NSAutoreleasePool.mm)
// and the retain-count side table's lock (NSObject.mm) — and all three asked the question as
// `#if defined(__EMSCRIPTEN_PTHREADS__)`. That was the right question for two targets and the
// wrong question in general: it is FALSE on every native build, including the ones that run
// `Classes/World.mm`'s world-load pthread and `Classes/MeshPool.mm`'s worker pool unconditionally.
// So on Linux the dispatch cache and the pool stack were process-global while two threads used
// them, and the engine died with SIGSEGV inside `objc_msg_lookup_sender`, called from
// `-[NSAutoreleasePool release]` on the main thread, while the load thread sat in
// `fmh_decodeColumnBands`. Intermittent, as races are: the same commit passed 64z and failed
// 256z on one run and the reverse on the next.
//
// The honest predicate is the negative one: a build is single-threaded ONLY when it is Emscripten
// WITHOUT pthreads. Native always has real threads — that is stated as an upside in
// native/CMakeLists.txt's link section ("Native has real pthreads, so World.mm's load thread and
// MeshPool.mm's worker pool both work unmodified"), which is exactly why the shim must assume it.
//
// Written as one shared macro rather than four copies of the same `#if` so it can only be got
// right or wrong once. Web's single-threaded codegen is unchanged: the macro is 0 in precisely
// the case the old condition was false.
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define EDEN_BUILD_HAS_THREADS 0
#else
#define EDEN_BUILD_HAS_THREADS 1
#endif

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
#if defined(__EMSCRIPTEN__) || defined(_WIN32) || \
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
// CFAbsoluteTimeGetCurrent, and MeshTiming_web.mm's mesh/upload timers — all only need
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
// answers with it, and FileManager.mm builds every world path under it.
// `bundle` is the read-only asset root: NSBundle's -bundlePath/-resourcePath, and the base of the
// basename index NSBundle.mm builds over <bundle>/media.
//
// Both are absolute, without a trailing slash, and stay valid for the process lifetime.
const char* eden_platform_documents_root(void);
const char* eden_platform_bundle_root(void);
// Native entry points call this before anything touches the filesystem (i.e. before
// eden_seam_main()). Passing NULL for either leaves that root at its default. No-op after the
// first filesystem access, so it is a startup switch, not a runtime one.
void eden_platform_set_roots(const char* documents, const char* bundle);

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
// (NSUserDefaults_native.mm has the same problem and solves it with SDL_CreateDirectory, which it
// can — nothing under web/src/shim/foundation may depend on SDL, because the web build has none.)
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
