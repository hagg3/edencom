# The Hand-Written ObjC Runtime Shim

Not in the root docs at all — Emscripten ships no Objective-C runtime, so this port
carries its own. Split out from [third-party.md](third-party.md) because it's dense
and referenced from several other docs (execution-flow, conventions-and-pitfalls).

## ABI choice
`-fobjc-runtime=gnustep-2.0` fails to compile for wasm — it needs ELF/COFF section
features the wasm object format doesn't have. Measured (not guessed) that
`gnustep-1.9`, `gnustep-1.8`, `gcc`, and `objfw` all compile; **gnustep-1.9** was
picked as the newest of the working set. Paired flags:
`-fconstant-string-class=NSConstantString -fno-objc-arc` (manual retain/release,
matching the root engine's own pre-ARC style).

`src/shim/objc/objc_abi.h` + `objc_runtime.cpp` implement this ABI from scratch.

## Dispatch model
**Slot-based**, not IMP-based: `objc_msg_lookup_sender`/`objc_slot_lookup_super`
return a `struct objc_slot*`. Every `[obj msg]` in the engine goes through
`lookupSlot()`, and `Hud.mm` alone issues 275 sends, so the send path has been
optimized in three measured steps. In the order a send meets them:

1. **A direct-mapped inline cache** (4096 entries, keyed on the low bits of
   `(class, interned selector name)`) — perf-audit row #15. One array read and one
   compare for the overwhelmingly common repeat send.
2. **An `unordered_map<uint64, slot*>` method cache** behind it, which is where the
   inline cache's misses and collisions land. The 64-bit key packs the two 32-bit
   wasm pointers exactly, so a hit needs no re-verification.
3. **A full class-chain walk** (`findMethod`) on first sight of a (class, selector)
   pair. No implementation anywhere ⇒ `unresolvedMethod()` names the selector and
   aborts; this runtime has no forwarding.

All caches are **per-thread** (`EDEN_OBJC_TLS`), not locked — the method lists they
read are immutable after static-constructor time, so two threads independently
reconstruct identical slots rather than contending. See the long comment above
`methodCache()`.

**The `isResolved()` guard runs AFTER the inline cache, and that ordering is
load-bearing** (ROADMAP B7). The guard catches a message sent to a class whose
superclass never registered — walking such a class's chain reads a `const char*`
name as if it were a pointer — but `resolvedClasses()` is insert-only, so a cache
hit is itself proof the class was resolved. An unresolved class can never hit;
it misses and gets the same named abort it always did. Moving the guard behind
the hit removed an `unordered_map` lookup from *every* send.

**`objc_lookup_class()` is on the hot path too**, and is easy to forget: clang emits
one at every `[ClassName msg]` site. `classesByName()` is keyed on `std::string`,
which pre-C++20 has no heterogeneous lookup, so `find(const char *)` used to build
and hash a `std::string` per class-message send (plus malloc/free for any name past
the 15-byte SSO buffer). `findClass()` now front-ends it with a 256-entry
direct-mapped cache keyed on the caller's name pointer, verified with `strcmp` on
hit and invalidated wholesale by a generation counter that `__objc_exec_class()`
bumps.

Measured (B7, 2026-08-28, `tools/headless-column-read-bench.js` — the port's
lowest-noise shim-level harness): **isolated column read 1.15 → 1.075 µs/column
(−7%) single-threaded, 2.84 → 2.73 (−4%) threaded**; in a whole-session CPU profile
`lookupSlot` + `findClass` self time fell **16.5 ms → 6.7 ms (−60%)**. The strcmp
verification was A/B'd against trusting the pointer outright and cost ~1.5%, inside
the harness's noise — so the safe version is the one that shipped.

## The ivar-layout sentinel fix (pass 7)
Under `-x objective-c++`, a class whose **entire ancestor chain has zero own
ivars** gets a `-1` sentinel first-ivar offset and a `+1`-biased `instance_size`
from the compiler. The runtime detects this sentinel and applies "+1 to own-size
and every offset" to compensate. **Not handled**: a class with own ivars sitting
after a superclass that *also* has own ivars — re-measure if you hit that shape;
don't assume the existing fix generalizes.

Reproduce any ivar-layout measurement yourself rather than trusting memory:
```
em++ -x objective-c++ -fobjc-runtime=gnustep-1.9 -fno-objc-arc -S -emit-llvm -o - FILE.mm | grep ...
```
(see `archive/PORT-STATUS-2026-08-13.md` for the exact grep pattern used historically). This is the
concrete instance of [conventions-and-pitfalls.md](conventions-and-pitfalls.md)'s
"measure, don't reason" rule — the ivar bug was mis-diagnosed at least once before
being measured directly.

## Constant strings and NSObject cluster
- `NSObject` — zero ivars; retain counts live in a side table, not inline.
- `NSString` — zero-ivar abstract base.
- `NSConstantString` — three words: `{isa, const char*, unsigned length}`, backing
  the 743 `@"..."` literals in the engine.
- `EdenConcreteString` — `std::string`-backed concrete subclass for runtime-built
  strings.

## C++ ivars are neither constructed nor destroyed — every class that has one owes a `-dealloc`
This runtime emits no `.cxx_construct`/`.cxx_destruct`: `class_createInstance()` is a `calloc`
and `object_dispose()` is a bare `free()`. A C++ ivar therefore starts life as an all-zero object
(fine in practice — a zeroed `std::vector` reads as a valid empty one, a zeroed `std::string` as a
valid empty short string) and **dies without its destructor running, leaking whatever it had on the
heap**. Nothing warns; the class works perfectly and just grows the heap.

**Rule: any shim class with a non-POD ivar must implement `-dealloc` that explicitly destroys it
and then calls `[super dealloc]`** (destroy before the storage is freed):
```objc
- (void)dealloc {
    _bytes.~eden_byte_vector();   // a named typedef, so the destructor can be spelled
    [super dealloc];
}
```
This has now bitten the port four times, each time found only by measurement:
`NSUserDefaults` (a `std::unordered_map` that also *crashed*, since an all-zero one is not valid),
`NSAutoreleasePool` (fixed by holding the vector behind a pointer + `delete`), and then
`NSData`/`NSMutableData`, `NSArray`/`NSSet` and `EdenConcreteString`/`NSMutableString` — the last
three found by ROADMAP Phase M / M6, where together they were **the ~22 MB every world load leaked**
(the texture loader's ~25 `+dataWithContentsOfFile:` PNG buffers and the column streamer's ~48 KB
`NSMutableData` per bundled-map column). `tools/headless-alloc-leak-probe.js` is the harness that
found them and the one to re-run if a new shim class grows a C++ ivar.

Two corollaries worth keeping:
- **`-dealloc` on the base class is enough**; mutable subclasses inherit it. Don't add a second one.
- **Destroy the container, not its contents.** `NSArray`/`NSSet` here do not retain what is put in
  them, so releasing their elements in `-dealloc` would over-release.

## `RuntimeError: function signature mismatch`
Under this ABI, this almost always means **a message was sent to a class with no
`@implementation`** (a missing `@implementation` still links fine — it just
dispatches into nothing) — check that first before suspecting a bad function
prototype. See [conventions-and-pitfalls.md](conventions-and-pitfalls.md).
