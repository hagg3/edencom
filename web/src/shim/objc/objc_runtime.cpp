// objc_runtime.cpp — the minimal Objective-C runtime backing the web port's Foundation shim.
//
// WHY THIS EXISTS: Emscripten ships no libobjc at all (pass-4 finding — no headers, no library,
// nothing in emsdk's system/ or cache/). Clang will happily COMPILE Objective-C for wasm, but
// every class it emits needs a runtime to register it and every `[obj msg]` it emits calls a
// dispatch entry point that does not exist. web-port-plan.md's blocker #1 weighed vendoring
// ObjFW (a) or GCC's libobjc (b) against writing this (c), and chose (c): the needed surface is
// small and closed, whereas (a)/(b) each mean porting a foreign build system into Emscripten —
// the same risk class as the toolchain spike but for a codebase this port does not control.
//
// SCOPE — deliberately, and this is the whole reason (c) was affordable. The engine and shim use
// plain classes and message sends only: no ARC, no ObjC exceptions, no KVO, no properties, no
// +load/+initialize (grep-verified across Classes/ and src/shim/), and the only two categories in
// the tree are in seam-excluded files (FileUpload, Appirater) though category merging is
// implemented anyway since it is a dozen lines. Anything outside that will fail loudly here
// rather than silently — see unresolvedMethod().
//
// The ABI this implements is documented and MEASURED in objc_abi.h; read that file first, and in
// particular its notes (1) slot-based dispatch, (2) negative instance_size + superclass-relative
// ivar offsets, and (3) super_class arriving as a name string. Those three are where a
// from-memory implementation of "the GNU runtime" would have been wrong.
//
// THREADING — resolved for the threaded build (audit row 36/C1, pass 63). The old note here said
// "if D1's PROXY_TO_PTHREAD build ever sends messages from it, the cache below needs a lock."
// It does send messages from it — Classes/World.mm's `loadWorldThread` opens with
// `[[NSAutoreleasePool alloc] init]` and `Terrain::loadTerrain` goes on to use NSString,
// NSFileHandle and NSBundle — so this is now a live constraint rather than a hypothetical. The
// resolution is NOT a lock; see the note above methodCache(). In one paragraph:
//
//   * Everything the dispatch path READS is immutable after static-ctor time. Registration
//     (`__objc_exec_class`) runs only from static constructors, which Emscripten runs once, on
//     the main thread, before main() — worker pthreads share that memory and never re-run them.
//     Nothing in this tree calls sel_registerName/objc_allocateClassPair/class_addMethod at
//     runtime (grep-verified across Classes/, *.mm and web/src/), so classesByName(),
//     internedNames(), canonicalSelectors(), resolvedClasses() and every method list are
//     read-only by the time a second thread exists. Concurrent reads of a settled
//     std::unordered_map are safe.
//   * The only dispatch-path WRITES are the two caches, and both are per-thread under -pthread.
#include "objc_abi.h"
#include "../foundation/platform_shims.h"   // EDEN_BUILD_HAS_THREADS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace {

// --- Selector interning ------------------------------------------------------------------
// A SEL is a pointer to an entry in some translation unit's `.objc_selector_list` (objc_abi.h),
// so the SAME message has a DIFFERENT SEL address in every .mm file. Dispatch therefore cannot
// compare SEL pointers; it compares the interned `name` pointer, which registration rewrites so
// that all spellings of "drawRect:" share one `const char *`. That keeps comparison a single
// pointer compare (no strcmp on the dispatch path) while staying correct across TUs.
std::unordered_map<std::string, const char *> &internedNames() {
  static std::unordered_map<std::string, const char *> m;
  return m;
}

const char *internName(const char *name) {
  std::unordered_map<std::string, const char *> &m = internedNames();
  std::unordered_map<std::string, const char *>::iterator it = m.find(name);
  if (it != m.end()) return it->second;
  // The key's own buffer is the canonical storage: std::string never reallocates a node in an
  // unordered_map, so `.c_str()` is stable for the process lifetime.
  const char *canonical = m.insert(std::make_pair(std::string(name), (const char *)0))
                              .first->first.c_str();
  m[name] = canonical;
  return canonical;
}

// One canonical selector object per name, for sel_registerName()/sel_getName() and for the SELs
// written back into method lists.
std::unordered_map<const char *, eden_objc_selector *> &canonicalSelectors() {
  static std::unordered_map<const char *, eden_objc_selector *> m;
  return m;
}

eden_objc_selector *canonicalSelector(const char *name, const char *types) {
  const char *interned = internName(name);
  std::unordered_map<const char *, eden_objc_selector *> &m = canonicalSelectors();
  std::unordered_map<const char *, eden_objc_selector *>::iterator it = m.find(interned);
  if (it != m.end()) {
    if (!it->second->types && types) it->second->types = types;
    return it->second;
  }
  eden_objc_selector *sel = new eden_objc_selector;
  sel->name = interned;
  sel->types = types;
  m[interned] = sel;
  return sel;
}

// Per-thread storage for the three dispatch-path caches (the class-name cache below and the two
// method caches further down). `thread_local` in any build that has threads, nothing at all in the
// single-threaded one — so the ST build's codegen is byte-for-byte what it was. The reasoning for
// per-thread rather than locked caches is spelled out above methodCache().
//
// This used to ask `#if defined(__EMSCRIPTEN_PTHREADS__)`, which is false on every NATIVE build
// even though native runs the world-load pthread and the mesh worker pool. That made these caches
// process-global under real concurrency and crashed the Linux leg inside this very function. See
// EDEN_BUILD_HAS_THREADS in platform_shims.h for the full account.
#if EDEN_BUILD_HAS_THREADS
#define EDEN_OBJC_TLS thread_local
#else
#define EDEN_OBJC_TLS
#endif

// --- Class registry ----------------------------------------------------------------------
std::unordered_map<std::string, eden_objc_class *> &classesByName() {
  static std::unordered_map<std::string, eden_objc_class *> m;
  return m;
}

// Classes whose superclass has not been registered yet. A module's classes can reference a
// superclass defined in a translation unit whose static constructor has not run — static ctor
// order across TUs is unspecified — so resolution is retried after every module load rather than
// assumed to succeed on first sight.
std::vector<eden_objc_class *> &pendingClasses() {
  static std::vector<eden_objc_class *> v;
  return v;
}

std::vector<eden_objc_category *> &pendingCategories() {
  static std::vector<eden_objc_category *> v;
  return v;
}

std::vector<eden_objc_static_instances *> &pendingStatics() {
  static std::vector<eden_objc_static_instances *> v;
  return v;
}

// A class is "resolved" once its superclass pointer, instance_size, ivar offsets and metaclass
// links have been patched. Tracked in a side set rather than in the class's `info` bits: the
// meanings of the GNU info flags beyond _CLS_CLASS/_CLS_META are not part of what objc_abi.h
// measured, and inventing a bit there risks colliding with something clang already means by it.
std::unordered_map<eden_objc_class *, bool> &resolvedClasses() {
  static std::unordered_map<eden_objc_class *, bool> m;
  return m;
}

bool isResolved(eden_objc_class *cls) {
  return cls && resolvedClasses().count(cls) != 0;
}

// Bumped by every __objc_exec_class(); see classCache() for why the cache keys on it.
unsigned &registryGeneration() {
  static unsigned g = 0;
  return g;
}

// Direct-mapped cache in front of classesByName() (ROADMAP B7, found by B6's profile). Clang emits
// an objc_lookup_class() call at EVERY `[ClassName msg]` site, so this runs once per class-message
// send — and `classesByName()` is keyed on std::string, which before C++20 has no heterogeneous
// lookup: `m.find(name)` therefore CONSTRUCTED a std::string from the `const char *` on every send,
// hashed the whole name, and for any name longer than the 15-byte SSO buffer ("NSAutoreleasePool",
// "NSMutableDictionary", …) called malloc and free as well. In B6's isolated read bench findClass
// was 3.9% of the whole column-read path's self time, next to nothing of which was the actual
// lookup. Keying on the caller's `const char *` skips all of that.
//
// Two things make the pointer a legitimate key rather than a shortcut:
//   * A HIT IS VERIFIED, not trusted — strcmp against the class's own name. A `const char *` is
//     not a stable identity in general (a freed heap string can be reallocated at the same
//     address), and this file's whole style is to not rely on an invariant it hasn't checked. The
//     strcmp is on a ~10–20 byte name that is already in cache; what it replaces is a string
//     construction plus a hash of the same bytes, so it is strictly cheaper than the miss path.
//   * The generation counter covers registration. classesByName() is insert-only AFTER static
//     construction (nothing in this tree calls objc_allocateClassPair — see the threading note at
//     the top of the file), but DURING it a name can be looked up before its class registers, and
//     `operator[]` would let a duplicate name overwrite. Rather than reason about either, every
//     __objc_exec_class() invalidates the whole cache by bumping the counter — free, since that
//     runs only from static constructors, and it means the cache cannot outlive a registry change.
// A miss falls through to the map unchanged; like the inline method cache, this is purely
// additive and can never be a second source of truth.
struct ClassCacheEntry {
  const char *name;
  eden_objc_class *cls;
  unsigned generation;
};
const unsigned kClassCacheSize = 256;  // power of two; the port registers ~40 classes
const unsigned kClassCacheMask = kClassCacheSize - 1;
ClassCacheEntry *classCache() {
  static EDEN_OBJC_TLS ClassCacheEntry table[kClassCacheSize] = {};
  return table;
}
inline unsigned classCacheIndex(const char *name) {
  // Name literals are byte-aligned and clustered per translation unit, so mix several byte
  // positions rather than taking the low bits straight.
  const size_t p = (size_t)name;
  return (unsigned)((p ^ (p >> 8) ^ (p >> 16)) & kClassCacheMask);
}

eden_objc_class *findClass(const char *name) {
  if (!name) return 0;
  ClassCacheEntry &ce = classCache()[classCacheIndex(name)];
  if (ce.name == name && ce.generation == registryGeneration() && ce.cls &&
      strcmp(ce.cls->name, name) == 0) {
    return ce.cls;
  }
  std::unordered_map<std::string, eden_objc_class *> &m = classesByName();
  std::unordered_map<std::string, eden_objc_class *>::iterator it = m.find(name);
  if (it == m.end()) return 0;  // misses are NOT cached — the class may register later
  ce.name = name;
  ce.cls = it->second;
  ce.generation = registryGeneration();
  return it->second;
}

void registerMethodList(eden_objc_method_list *list) {
  for (eden_objc_method_list *l = list; l; l = l->next) {
    for (int i = 0; i < l->count; i++) {
      // As emitted, `name` is a plain `const char *`; it is replaced in place by the canonical
      // SEL so dispatch can compare interned name pointers. See objc_abi.h's eden_objc_method.
      const char *name = (const char *)l->methods[i].name;
      l->methods[i].name = canonicalSelector(name, l->methods[i].types);
    }
  }
}

// Returns false if the superclass is not registered/resolved yet — caller retries later.
bool resolveClass(eden_objc_class *cls) {
  if (!cls) return true;
  if (isResolved(cls)) return true;

  // objc_abi.h note (3): this field holds the superclass NAME until now.
  const char *superName = (const char *)cls->super_class;
  eden_objc_class *super = 0;
  if (superName) {
    super = findClass(superName);
    if (!super) return false;
    if (!isResolved(super) && !resolveClass(super)) return false;
  }

  const long superSize = super ? super->instance_size : 0;

  // *** OBJECTIVE-C++ MODE (`-x objective-c++`, what this build uses) EMITS A DIFFERENT, MORE
  // BIASED ENCODING THAN PLAIN OBJECTIVE-C, BUT ONLY FOR A CLASS WHOSE ENTIRE ANCESTOR CHAIN UP
  // TO THE ROOT HAS ZERO OWN IVARS. *** Measured with real `em++ -x objective-c++` IR across
  // 1-, 2- and 3-level all-empty chains (NSObject direct child; NSObject → empty-subclass →
  // ivar-bearing class, matching this port's NSObject → NSString → EdenConcreteString/
  // NSConstantString shape): in that situation clang's C++ empty-base-class-optimization view of
  // the hierarchy biases BOTH `instance_size` and EVERY emitted ivar offset by exactly one byte
  // versus plain ObjC, and — cheaply, losslessly — the class's FIRST ivar offset is the literal
  // sentinel `-1` if and only if this bias is in play; it is never `-1` otherwise (measured: a
  // class whose immediate superclass already has real ivars of its own emits ordinary
  // non-negative first-ivar offsets in both compile modes, byte-for-byte identical between them).
  // The correction is "+1 to instance_size's own-size term, +1 to every ivar offset" — nothing
  // more. Internal padding between this class's OWN ivars is otherwise already correct as
  // emitted (verified against a `{double d; int e;}` / `{int e; double d;}` pair — the relative
  // deltas between successive ivars match a real 8-byte-aligned C layout exactly). The resulting
  // *absolute* offset (superSize + biased relative offset) is not always a multiple of the
  // ivar's natural alignment when sizeof(id) itself isn't (e.g. a `double` straight after just a
  // 4-byte `isa` lands at absolute offset 4, not 8) — this is fine and intentionally left as-is:
  // WebAssembly's f64/i64 load/store have no alignment requirement for correctness, only a minor
  // performance hint, and both the ivar_list entry and ivar_offsets global are patched to the
  // same value below, so every reader of the ivar agrees regardless. Do not "fix" this with
  // padding; it isn't broken on this target.
  //
  // This does NOT occur anywhere else in the shim today (no class both has its own ivars AND
  // subclasses another class that also has its own ivars), so that combination isn't handled —
  // re-measure (see objc_abi.h's reproduction command) before extending this to such a class.
  const bool cppEmptyBaseBias =
      cls->ivars && cls->ivars->count > 0 && cls->ivars->ivars[0].offset == -1;

  // objc_abi.h note (2): |instance_size| covers only this class's OWN ivars, and ivar offsets are
  // relative to the end of the superclass. Both become absolute here. Doing this twice would
  // silently double every offset, which is exactly why isResolved() guards the whole function.
  cls->instance_size = -cls->instance_size + (cppEmptyBaseBias ? 1 : 0) + superSize;

  // *** THE IMPLICIT `isa` IS NOT IN THE EMITTED SIZE FOR A ZERO-IVAR ROOT CLASS. ***
  // Measured: `@interface Base { Class isa; }` emits -4, but `@interface NSObject { }` — no ivar
  // block at all, which is exactly what this port's NSObject is (its zero-ivar layout is
  // load-bearing for the @"literal" constant-string class, see NSString.h) — emits 0 or -1.
  // The isa pointer still occupies the first word of every instance regardless, so a root class
  // is never smaller than one pointer.
  //
  // Getting this wrong under-allocated EVERY object by 4 bytes, since the error propagates down
  // the whole hierarchy: NSObject came out as 1 instead of 4, so NSConstantString resolved to 8
  // instead of the 12 its three-word layout requires, and EdenConcreteString's std::string ran
  // off the end of its allocation. The symptom was a "memory access out of bounds" inside
  // dispatch, several sends away from the actual cause.
  if (!super && cls->instance_size < (long)sizeof(id)) {
    cls->instance_size = (long)sizeof(id);
  }

  if (superSize != 0 && cls->ivars) {
    for (int i = 0; i < cls->ivars->count; i++) {
      cls->ivars->ivars[i].offset += (cppEmptyBaseBias ? 1 : 0) + (int)superSize;
      // The `ivar_offsets` array holds pointers to separate `__objc_ivar_offset_value_*` globals
      // that duplicate the same numbers; real ivar accesses read through the ivar_list entry
      // above (measured — see objc_abi.h's closing note), but both must agree.
      if (cls->ivar_offsets && cls->ivar_offsets[i]) {
        *cls->ivar_offsets[i] += (cppEmptyBaseBias ? 1 : 0) + (int)superSize;
      }
    }
  }

  cls->super_class = super;

  // Metaclass links. Clang emits the metaclass with a null isa and null super_class and expects
  // the runtime to wire both: a metaclass's superclass is the superclass's metaclass, and the
  // ROOT metaclass's superclass is the root CLASS — that last link is what makes NSObject's
  // instance methods (-respondsToSelector:, -class, …) reachable when the receiver is a Class.
  if (cls->isa) {
    eden_objc_class *root = cls;
    while (root->super_class) root = root->super_class;
    cls->isa->super_class = super ? super->isa : root;
    cls->isa->isa = root->isa;
  }

  resolvedClasses()[cls] = true;
  // The METACLASS must be marked too, not just the class. A message to a Class object dispatches
  // through `(*receiver)->isa`, which IS the metaclass — so anything keyed on "is this resolved?"
  // sees the metaclass, never the class. Marking only the class made every `[SomeClass method]`
  // look unresolved.
  if (cls->isa) resolvedClasses()[cls->isa] = true;
  return true;
}

void applyCategory(eden_objc_category *cat) {
  eden_objc_class *cls = findClass(cat->class_name);
  if (!cls) return;  // includes the compiler's "__ObjC_Protocol_Holder_Ugly_Hack" fake category
  if (cat->instance_methods) {
    registerMethodList(cat->instance_methods);
    // Prepended, so category methods win over the class's own — matching the GNU runtime, where
    // a category replaces an existing implementation of the same selector.
    cat->instance_methods->next = cls->methods;
    cls->methods = cat->instance_methods;
  }
  if (cat->class_methods && cls->isa) {
    registerMethodList(cat->class_methods);
    cat->class_methods->next = cls->isa->methods;
    cls->isa->methods = cat->class_methods;
  }
}

// objc_abi.h: this is the `@"literal"` path — 743 sites in the engine depend on it.
bool applyStatics(eden_objc_static_instances *statics) {
  eden_objc_class *cls = findClass(statics->class_name);
  if (!cls) return false;
  for (int i = 0; statics->instances[i]; i++) {
    statics->instances[i]->isa = (Class)cls;
  }
  return true;
}

// Re-attempt everything deferred. Cheap: the pending lists are empty in the steady state, and
// this only runs during static construction.
void drainPending() {
  bool progress = true;
  while (progress) {
    progress = false;

    for (size_t i = 0; i < pendingClasses().size();) {
      if (resolveClass(pendingClasses()[i])) {
        pendingClasses().erase(pendingClasses().begin() + i);
        progress = true;
      } else {
        i++;
      }
    }

    for (size_t i = 0; i < pendingStatics().size();) {
      if (applyStatics(pendingStatics()[i])) {
        pendingStatics().erase(pendingStatics().begin() + i);
        progress = true;
      } else {
        i++;
      }
    }

    for (size_t i = 0; i < pendingCategories().size();) {
      if (findClass(pendingCategories()[i]->class_name)) {
        applyCategory(pendingCategories()[i]);
        pendingCategories().erase(pendingCategories().begin() + i);
        progress = true;
      } else {
        i++;
      }
    }
  }
}

// --- Dispatch ----------------------------------------------------------------------------
// PER-THREAD CACHES, not locked caches (audit row 36/C1, pass 63). Both caches below — and the
// class-name cache up in the class registry — are `EDEN_OBJC_TLS`, which is `thread_local` in the
// threaded build and nothing at all in the single-threaded one (the macro is defined above
// classesByName()), so the ST build's codegen is byte-for-byte what it was.
//
// Why per-thread beats a mutex here, and it is not close: this is the hot path of every single
// `[obj msg]` in the engine — Hud.mm alone has 275 sends and per-frame sends run into the
// thousands (that count is why the inline cache below exists at all). A mutex would put an
// atomic RMW pair on every one of them, on the main thread, to protect against a background
// thread that sends a few hundred messages during a world load. Per-thread caches put ZERO
// instructions on the hot path: each thread simply misses into the shared, immutable method
// lists the first time it sends a given (class, selector) pair, and hits its own cache after.
//
// The cost is duplication, and it is provably harmless: two threads that both look up the same
// (cls, sel) each `new` their own eden_objc_slot with IDENTICAL contents, because the method
// list they read it from is immutable. Nothing anywhere compares slots by identity — clang loads
// field index 4 (`method`) and calls it, and `owner`/`cachedFor`/`version` are written but never
// read (objc_abi.h's own note on the struct). So the duplicate is a few dozen bytes per
// (thread, class, selector) actually used off the main thread, bounded and small — the world
// load touches a handful of Foundation classes, not the whole engine.
//
// If a future thread ever DOES need to see main-thread cache state, the answer is still not a
// lock on this path — it is that the underlying method lists are already shared, so the second
// thread reconstructs the same answer on its own.
// Method cache, keyed on (class, interned selector name). Both are 32-bit under wasm32, so the
// packed 64-bit key is exact — no collisions, no need to re-verify on hit.
std::unordered_map<unsigned long long, eden_objc_slot *> &methodCache() {
  static EDEN_OBJC_TLS std::unordered_map<unsigned long long, eden_objc_slot *> m;
  return m;
}

inline unsigned long long cacheKey(eden_objc_class *cls, const char *selName) {
  return ((unsigned long long)(unsigned int)(size_t)cls << 32) |
         (unsigned long long)(unsigned int)(size_t)selName;
}

// Direct-mapped inline cache in front of methodCache()'s hash map (perf-audit row #15): the map
// lookup is an `unordered_map<uint64,...>::find` per message send, and `Hud.mm` alone has 275
// sends, so per-frame sends are plausibly in the thousands. A small fixed-size, power-of-two table
// keyed on the low bits of `cacheKey` turns the overwhelming majority of repeat sends (same class,
// same selector, which is the common case in a game loop) into one array read + one compare.
// A miss (collision or first-ever send of that (cls,sel) pair) falls through to the hash map
// unchanged — this is purely additive, never a second source of truth: eviction from the direct
// map can never make a lookup wrong, only fail to speed it up.
struct InlineCacheEntry {
  unsigned long long key;
  eden_objc_slot *slot;
};
const unsigned kInlineCacheSize = 4096;  // power of two
const unsigned kInlineCacheMask = kInlineCacheSize - 1;
// EDEN_OBJC_TLS: 32 KB of table per thread in the threaded build (see the note above
// methodCache()). With -sPTHREAD_POOL_SIZE=4 that is 160 KB of a ~100 MB heap — cheap next to
// putting a lock on the send path.
InlineCacheEntry *inlineCache() {
  static EDEN_OBJC_TLS InlineCacheEntry table[kInlineCacheSize] = {};
  return table;
}
inline unsigned inlineCacheIndex(unsigned long long key) {
  // Selector pointers dominate the low bits (they're interned `const char*`s from a table that
  // rarely exceeds a few hundred entries); mixing in the upper (class) bits keeps distinct classes
  // sending the same selector from all colliding on the same slot.
  return (unsigned)((key ^ (key >> 32)) & kInlineCacheMask);
}

eden_objc_method *findMethod(eden_objc_class *cls, const char *selName) {
  for (eden_objc_class *c = cls; c; c = c->super_class) {
    for (eden_objc_method_list *l = c->methods; l; l = l->next) {
      for (int i = 0; i < l->count; i++) {
        if (((eden_objc_selector *)l->methods[i].name)->name == selName) return &l->methods[i];
      }
    }
  }
  return 0;
}

// Nil-receiver handler. Deliberately NOT variadic: wasm validates indirect calls against the exact
// function signature, and a variadic `id(id, SEL, ...)` compiles to a 3-param signature (the extra
// param being the varargs buffer pointer). A plain nil send like `[storedPaintMask CGImage]` calls
// through a 2-param signature, so the variadic version trapped with "function signature mismatch"
// (seen in Resources::getPaintTex, whose storedPaint/storedPaintMask globals are never assigned).
// A 2-param (id, SEL) shape matches every zero-argument nil send, which is what the engine does.
// Caveat: a nil send that PASSES arguments still mismatches; the fully general fix would be
// -sEMULATE_FUNCTION_POINTER_CASTS=1, which costs size/perf everywhere, so it is not used here.
id nilMethod(id self, SEL) { return self; }

eden_objc_slot *nilSlot() {
  static eden_objc_slot slot = {0, 0, 0, 0, (IMP)nilMethod};
  return &slot;
}

// A message with no implementation. The real GNU runtime would enter forwarding here; this port
// has no forwarding (nothing in the engine or shim implements -forwardInvocation:), so the honest
// thing is to fail loudly and name the receiver — a returned null IMP would be called immediately
// and crash somewhere far away with no clue which selector was missing.
eden_objc_slot *unresolvedMethod(eden_objc_class *cls, const char *selName) {
  fprintf(stderr,
          "eden objc runtime: unrecognized selector -[%s %s] "
          "(no implementation found in the class chain; this runtime does not forward)\n",
          cls && cls->name ? cls->name : "<null class>", selName ? selName : "<null selector>");
  abort();
  return nilSlot();
}

eden_objc_slot *lookupSlot(eden_objc_class *cls, eden_objc_selector *sel) {
  const char *selName = sel->name;
  const unsigned long long key = cacheKey(cls, selName);

  // The inline cache comes FIRST, ahead of the isResolved() guard below (ROADMAP B7). The guard
  // used to run on every single message send, which made an `unordered_map<ptr,bool>` lookup the
  // unavoidable floor of the fastest path in the runtime — B6's profile caught it inside
  // lookupSlot, 5.1% of the whole column-read path. Reordering keeps the guard's diagnostic value
  // intact: `resolvedClasses()` is insert-only (never erased — see resolveClass()), so a class
  // that was resolved when this entry was cached is still resolved now, and a cache HIT is itself
  // proof that this (class, selector) pair reached the miss path below and passed the guard there.
  // An unresolved class can therefore never hit; it misses (nothing ever cached for it) and gets
  // the same named abort it always did, on the same send. Computing `key` before the guard is
  // safe — it only reads the pointers, never dereferences `cls`, which is the whole hazard.
  InlineCacheEntry &ic = inlineCache()[inlineCacheIndex(key)];
  if (ic.key == key && ic.slot) return ic.slot;

  // Guard, not an assertion for its own sake: an UNRESOLVED class still holds a `const char *`
  // class NAME in its super_class field (objc_abi.h note 3), so walking the chain would step off
  // the end of a string and into arbitrary memory. That is precisely the "memory access out of
  // bounds" this check replaced, and it is worth keeping — it turns a class that failed to
  // register into a named error instead of a wild read.
  if (!isResolved(cls)) {
    fprintf(stderr,
            "eden objc runtime: message to unresolved class '%s' (superclass '%s' never "
            "registered; %zu classes still pending)\n",
            (cls && cls->name) ? cls->name : "<null>",
            (cls && cls->super_class) ? (const char *)cls->super_class : "<none>",
            pendingClasses().size());
    abort();
  }

  std::unordered_map<unsigned long long, eden_objc_slot *> &cache = methodCache();
  std::unordered_map<unsigned long long, eden_objc_slot *>::iterator it = cache.find(key);
  if (it != cache.end()) {
    ic.key = key;
    ic.slot = it->second;
    return it->second;
  }

  eden_objc_method *method = findMethod(cls, selName);
  if (!method) return unresolvedMethod(cls, selName);

  eden_objc_slot *slot = new eden_objc_slot;
  slot->owner = cls;
  slot->cachedFor = cls;
  slot->types = method->types;
  slot->version = 1;
  slot->method = method->imp;
  cache[key] = slot;
  ic.key = key;
  ic.slot = slot;
  return slot;
}

// --- ABI size guard (Phase N Stage 3.3) --------------------------------------------------
// objc_abi.h's structs were read off wasm32 IR, where sizeof(long) == sizeof(void*) makes three
// distinct field widths indistinguishable. Stage 3.3 re-measured them at LP64 and LLP64 and found
// one field pair that is pointer-width rather than `long`-width; on Windows that was an 8-byte
// struct-size error. Nothing READS those fields, so nothing would have crashed — the class
// initialisers would simply have been misread field-for-field on some future target, silently.
//
// The compiler hands us two `sizeof`s of its own, for free, so the disagreement is detectable
// instead of theoretical:
//   * every METACLASS's emitted `instance_size` is literally sizeof(struct objc_class), and
//   * the module's `size` field is sizeof(struct objc_module).
// Both are read before any resolution rewrites them. Checked once, on the first module, then
// never again — this is a startup check, not a per-module cost.
//
// It ABORTS rather than warns: a mismatch means every subsequent field read from a class struct
// is off by some number of bytes, which corrupts the heap several dispatches later with a
// symptom that names nothing. A message at startup naming the two numbers is the whole value.
void checkAbiSizes(eden_objc_module *module, eden_objc_symtab *symtab) {
  static bool checked = false;
  if (checked) return;
  checked = true;

  bool ok = true;
  if (module->size != (unsigned long)sizeof(eden_objc_module)) {
    fprintf(stderr,
            "[eden-objc] FATAL: compiler emits sizeof(objc_module) = %lu, objc_abi.h says %lu.\n",
            (unsigned long)module->size, (unsigned long)sizeof(eden_objc_module));
    ok = false;
  }
  // defs[0..cls_def_cnt) are classes; each carries its metaclass in `isa`. A categories-only
  // module has none, and simply defers the check to whichever module registers a class first.
  if (symtab->cls_def_cnt > 0) {
    eden_objc_class *cls = (eden_objc_class *)symtab->defs[0];
    if (cls && cls->isa && cls->isa->instance_size != (long)sizeof(eden_objc_class)) {
      fprintf(stderr,
              "[eden-objc] FATAL: compiler emits sizeof(objc_class) = %ld, objc_abi.h says %lu.\n",
              (long)cls->isa->instance_size, (unsigned long)sizeof(eden_objc_class));
      ok = false;
    }
  }
  if (!ok) {
    fprintf(stderr, "[eden-objc] The ABI structs in web/src/shim/objc/objc_abi.h do not match this\n"
                    "[eden-objc] target's codegen. Re-run that file's documented IR recipe for this\n"
                    "[eden-objc] target and fix the struct; do NOT relax this check.\n");
    abort();
  }
}

}  // namespace

extern "C" {

// Called from a static constructor per translation unit that contains Objective-C. Declared
// variadic by clang (`declare void @__objc_exec_class(ptr, ...)`), so it is DEFINED variadic too:
// under the wasm ABI a variadic function takes a hidden argument-buffer pointer, and a
// non-variadic definition would be a signature mismatch at link time.
void __objc_exec_class(void *module_ptr, ...) {
  eden_objc_module *module = (eden_objc_module *)module_ptr;
  if (!module || !module->symtab) return;
  eden_objc_symtab *symtab = module->symtab;

  checkAbiSizes(module, symtab);

  // The registry is about to change; invalidate every thread's class-name cache (see classCache()).
  // Only static constructors reach here, so this costs nothing on any hot path.
  registryGeneration()++;

  // Selectors first: method-list registration below interns against the same table.
  if (symtab->refs) {
    for (eden_objc_selector *s = symtab->refs; s->name; s++) {
      s->name = internName(s->name);
    }
  }

  unsigned int idx = 0;

  for (unsigned short i = 0; i < symtab->cls_def_cnt; i++, idx++) {
    eden_objc_class *cls = (eden_objc_class *)symtab->defs[idx];
    if (!cls) continue;
    registerMethodList(cls->methods);
    if (cls->isa) registerMethodList(cls->isa->methods);
    classesByName()[cls->name] = cls;
    pendingClasses().push_back(cls);
  }

  for (unsigned short i = 0; i < symtab->cat_def_cnt; i++, idx++) {
    eden_objc_category *cat = (eden_objc_category *)symtab->defs[idx];
    if (cat) pendingCategories().push_back(cat);
  }

  // Then, if present, a NULL-terminated array of static-instance lists (the @"literal" path).
  eden_objc_static_instances **statics = (eden_objc_static_instances **)symtab->defs[idx];
  if (statics) {
    for (int i = 0; statics[i]; i++) pendingStatics().push_back(statics[i]);
  }

  drainPending();
}

// --- Dispatch entry points clang actually emits ------------------------------------------
// Note the first parameter: a POINTER to the receiver, not the receiver. Clang stores the
// receiver into a stack slot and passes its address so a runtime with forwarding can substitute
// a different receiver; this one never does, but the signature is fixed by the compiler.
struct eden_objc_slot *objc_msg_lookup_sender(id *receiver, SEL sel, id sender) {
  (void)sender;
  if (!receiver || !*receiver) return nilSlot();
  return lookupSlot((eden_objc_class *)(*receiver)->isa, (eden_objc_selector *)sel);
}

// `super->super_class` is already the class to start searching from — for a `[super x]` in a
// class method clang emits the metaclass there, so no isa hop is needed (or wanted) here.
struct eden_objc_slot *objc_slot_lookup_super(struct objc_super *super, SEL sel) {
  if (!super || !super->receiver) return nilSlot();
  return lookupSlot((eden_objc_class *)super->super_class, (eden_objc_selector *)sel);
}

// Provided for completeness. web-port-plan.md's blocker #1 expected these two to BE the dispatch
// path; the measured codegen uses the slot-based pair above instead (objc_abi.h note 1). Nothing
// clang emits for this engine calls them — they exist so the declarations in objc/runtime.h have
// definitions, and so hand-written shim code that prefers an IMP has one.
IMP objc_msg_lookup(id receiver, SEL sel) {
  if (!receiver) return (IMP)nilMethod;
  return lookupSlot((eden_objc_class *)receiver->isa, (eden_objc_selector *)sel)->method;
}

IMP objc_msg_lookup_super(struct objc_super *super, SEL sel) {
  return objc_slot_lookup_super(super, sel)->method;
}

// Emitted at every `for (x in collection)` site (Input.mm's `for (UITouch *t in touches)` is the
// one in this tree) and called only if the collection's mutation counter changes mid-loop. That
// would be a genuine bug, but not one worth killing the frame over in a game loop — report it and
// keep going, unlike unresolvedMethod() where continuing means calling a null IMP.
void objc_enumerationMutation(id obj) {
  static bool warned = false;
  if (!warned) {
    warned = true;
    fprintf(stderr, "eden objc runtime: collection %p mutated while being enumerated\n",
            (void *)obj);
  }
}

// --- Class lookup -------------------------------------------------------------------------
// Clang emits objc_lookup_class() at every `[ClassName msg]` site, variadic per its declaration
// (`declare ptr @objc_lookup_class(ptr, ...)`) — same wasm signature-matching reason as
// __objc_exec_class above.
Class objc_lookup_class(const char *name, ...) { return (Class)findClass(name); }
Class objc_get_class(const char *name, ...) { return (Class)findClass(name); }
Class objc_getClass(const char *name) { return (Class)findClass(name); }

Class objc_getMetaClass(const char *name) {
  eden_objc_class *cls = findClass(name);
  return cls ? (Class)cls->isa : 0;
}

// --- The objc/runtime.h surface the Foundation shim calls ---------------------------------
id class_createInstance(Class cls, size_t extraBytes) {
  eden_objc_class *c = (eden_objc_class *)cls;
  if (!c) return 0;
  const size_t size = (size_t)c->instance_size + extraBytes;
  id obj = (id)calloc(1, size);
  if (obj) obj->isa = cls;
  return obj;
}

void object_dispose(id obj) { free(obj); }

Class object_getClass(id obj) { return obj ? obj->isa : 0; }

Class class_getSuperclass(Class cls) {
  eden_objc_class *c = (eden_objc_class *)cls;
  return c ? (Class)c->super_class : 0;
}

const char *class_getName(Class cls) {
  eden_objc_class *c = (eden_objc_class *)cls;
  return c && c->name ? c->name : "nil";
}

BOOL class_respondsToSelector(Class cls, SEL sel) {
  if (!cls || !sel) return NO;
  return findMethod((eden_objc_class *)cls, ((eden_objc_selector *)sel)->name) ? YES : NO;
}

SEL sel_registerName(const char *name) { return (SEL)canonicalSelector(name, 0); }

const char *sel_getName(SEL sel) {
  return sel ? ((eden_objc_selector *)sel)->name : "<null selector>";
}

}  // extern "C"
