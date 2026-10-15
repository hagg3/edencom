// objc_abi.h — the GNU Objective-C ABI structures clang emits under `-fobjc-runtime=gnustep-1.9`.
//
// *** EVERY LAYOUT HERE WAS MEASURED, NOT REMEMBERED. *** The web-port-plan's blocker #1 picked
// option (c) "hand-write a minimal runtime", and the one way that goes wrong is guessing a struct
// field the compiler actually emits differently — the corruption would be silent until a wrong
// dispatch at runtime. So each struct below was read off real emcc 3.1.74 LLVM IR:
//
//     emcc -x objective-c -fobjc-runtime=gnustep-1.9 -fno-objc-arc \
//          -fconstant-string-class=NSConstantString -S -emit-llvm -o - test.m
//
// Reproduce that on any toolchain bump and re-check this file against it before trusting it.
// The IR field lists are quoted verbatim above each struct; wasm32 widths apply (`long` and
// pointers are both 4 bytes, which is why clang prints `i32` where the classic GNU runtime
// headers say `long`).
//
// *** RE-MEASURED AT 64-BIT WIDTHS, PHASE N STAGE 3.3 (2026-09-05). *** Every struct here had
// only ever been read off wasm32 IR, where `sizeof(long) == sizeof(void*) == 4` collapses three
// distinct field widths into one — so a field that is really pointer-width and a field that is
// really `long`-width are indistinguishable there, and getting one wrong is silent. Re-read with
// the same recipe on `x86_64-linux-gnu` (LP64), `aarch64-linux-gnu` (LP64) and
// `x86_64-w64-windows-gnu` (LLP64), in `-x objective-c++`, which is the mode this port compiles:
//
//     wasm32   @_OBJC_CLASS_Foo = { ptr,ptr,ptr, i32,i32,i32, ptr x7, i32, ptr,ptr, i32,i32 }
//     LP64     @_OBJC_CLASS_Foo = { ptr,ptr,ptr, i64,i64,i64, ptr x7, i64, ptr,ptr, i64,i64 }
//     LLP64    @_OBJC_CLASS_Foo = { ptr,ptr,ptr, i32,i32,i32, ptr x7, i32, ptr,ptr, i64,i64 }
//     module     wasm32 { i32 9, i32 16, ptr, ptr }   LP64 { i64 9, i64 32, ptr, ptr }
//                LLP64  { i32 9, i32 24, ptr, ptr }
//     symtab     sel_ref_cnt tracks `long`: i32 on wasm32 AND on Windows, i64 on LP64.
//     selector / method / ivar lists: BYTE-IDENTICAL on all three (i32 counts, ptr payloads).
//
// Two results, one of which was a real (latent) bug:
//
//   * The `long`-typed fields — version/info/instance_size/abi_version, and the module and symtab
//     counts — all track the TARGET's `long`, so declaring them `long` is already right on every
//     target including Windows, where `long` is 4 bytes and clang duly emits i32.
//   * `strong_pointers`/`weak_pointers` DO NOT. They are POINTER-width (i32 on wasm32, i64 on
//     BOTH LP64 and LLP64), and were declared `long` here — which is right by coincidence on
//     wasm32 and LP64 and WRONG BY 8 BYTES OF STRUCT SIZE on Windows. Nothing reads those two
//     fields (they are GC layout bitmaps), so the only consequence is `sizeof`, but `sizeof` is
//     observable: see below. They are `void *` now, which is correct on all three by
//     construction rather than by luck.
//
// AND THE STRUCT SIZE IS CHECKABLE AT RUNTIME, WHICH IS WHY THIS IS NOT LEFT AS A COMMENT.
// Clang emits a METACLASS whose `instance_size` is literally `sizeof(struct objc_class)` as the
// compiler lays it out — 72 on wasm32, 144 on LP64, 136 on LLP64 (each exactly what the field
// list above adds up to, and the 136 is what proves the `void *` fix: with `long` there the
// struct would be 128). The module's `size` field is `sizeof(struct objc_module)` the same way.
// objc_runtime.cpp's __objc_exec_class() now compares both against this file's `sizeof` on the
// first module it sees and aborts with a diagnostic if they disagree, so a future target whose
// layout differs again fails at startup with a message instead of corrupting the heap.
//
// Three measurements were surprising enough to call out, because they contradict what the
// "classic GCC libobjc" folklore would lead you to write:
//
//   1. DISPATCH IS SLOT-BASED, NOT IMP-BASED. Clang emits calls to
//      `objc_msg_lookup_sender(id *receiver, SEL, id sender)` and `objc_slot_lookup_super`,
//      each returning a `struct objc_slot *` from which it loads field 4 (the IMP). It does NOT
//      call `objc_msg_lookup` (which web-port-plan.md's blocker #1 named) — that entry point is
//      still provided below for completeness, but nothing clang generates reaches it.
//
//   2. `instance_size` ARRIVES NEGATIVE AND IVAR OFFSETS ARRIVE SUPERCLASS-RELATIVE — BUT THE
//      EXACT ENCODING DIFFERS BETWEEN PLAIN OBJECTIVE-C AND OBJECTIVE-C++ (THIS BUILD).
//      For `@interface Base { Class isa; }` clang emits instance_size = -4; for
//      `@interface Sub : Base { int a; double b; }` it emits -12 with ivar offsets 0 and 4.
//      So |instance_size| is the size of the class's OWN ivars and the offsets are relative to
//      the end of the superclass — the runtime must add the resolved superclass instance_size to
//      both. Skip that fixup and every ivar in every subclass reads the wrong memory.
//
//      That description is exactly right for `-x objective-c`, and for ANY class in
//      `-x objective-c++` whose immediate superclass already has real ivars of its own. It is
//      WRONG by exactly one byte — in both instance_size and every ivar offset — for an
//      Objective-C++ class whose entire ancestor chain up to the root has zero own ivars (e.g.
//      any direct subclass of this port's zero-ivar NSObject; also NSString → EdenConcreteString/
//      NSConstantString, since NSString itself declares no ivars). Measured mechanism: clang's
//      C++ empty-base-class-optimization view of an all-empty ancestor chain. It is losslessly
//      detectable: the class's FIRST ivar offset is the literal sentinel `-1` if and only if this
//      bias is in play — never `-1` otherwise. See `resolveClass()` in objc_runtime.cpp for the
//      fix and the full measurement writeup. One more thing this sentinel obscures: the
//      *absolute* offset it produces is not always naturally aligned (a `double` straight after a
//      4-byte `isa` lands at absolute offset 4, not 8) — harmless on this target, since
//      WebAssembly's f64/i64 load/store have no alignment requirement for correctness and every
//      reader of the ivar goes through the same patched offset. Do not add padding to "fix" that.
//
//      *** ALL OF NOTE (2) RE-VERIFIED AT 64-BIT, STAGE 3.3, AND IT IS UNCHANGED. *** This was
//      the other silent-if-wrong item, because the `-1` sentinel is a detection mechanism and a
//      detector that stops firing is indistinguishable from "the bias went away". Measured on
//      LP64 (x86_64 + aarch64) and LLP64 with this port's exact class shapes — zero-ivar root
//      NSObject, zero-ivar abstract NSString, then the two ivar-bearing leaves:
//        - the sentinel is still the literal `-1`, on every target, for every all-empty chain;
//        - the control (a class whose immediate superclass has real ivars of its own) still
//          emits a non-negative first offset — `0, 8` — on every target, so the detector still
//          discriminates;
//        - the bias is still EXACTLY ONE BYTE, not one word. NSConstantString emits `-1, 7`
//          where the true relative offsets are `0, 8` on LP64 (`-1, 15` for a 24-byte first
//          ivar where the truth is `0, 16`). A word-sized bias would have been the natural
//          guess and it is wrong.
//      Absolute sizes fall out correct: NSObject 8, NSString 8, NSConstantString 24 on LP64,
//      which is `{ Class isa; const char *; unsigned; }` with its natural tail padding.
//
//   3. `super_class` ARRIVES AS A `const char *` CLASS NAME, not a Class pointer, and is
//      overwritten in place with the real Class during registration. Code emitted for `[super x]`
//      loads this field at RUNTIME (see objc_runtime.cpp's note on objc_slot_lookup_super), so
//      the in-place patch is not an optimization — it is what makes super sends work at all.
//
// Ivar access codegen (also measured) is a double indirection: clang loads the global
// `__objc_ivar_offset_Class.name`, which points INTO the objc_ivar_list entry's offset field,
// then loads the int from there. That is why the fixup in (2) must patch the ivar_list entries
// themselves — patching only the `ivar_offsets` array would leave real ivar accesses stale.
#ifndef EDEN_SHIM_OBJC_ABI_H
#define EDEN_SHIM_OBJC_ABI_H

#include <objc/objc.h>
#include <objc/runtime.h>

extern "C" {

// IR: `{ ptr name, ptr types }`, in a `.objc_selector_list` array terminated by a zeroed entry.
// A SEL is a POINTER TO ONE OF THESE ENTRIES — clang passes `getelementptr .objc_selector_list,
// i32 0, i32 N` as the selector argument. Two SELs for the same message therefore have different
// addresses in different translation units; see objc_runtime.cpp for how they are made
// comparable (name-pointer interning).
struct eden_objc_selector {
  const char *name;
  const char *types;
};

// IR: `{ ptr name, ptr types, ptr imp }`. `name` holds a `const char *` as emitted and is
// replaced in place by a registered SEL during module load — the same trick the GNU runtime uses.
struct eden_objc_method {
  void *name;             // const char* before registration, eden_objc_selector* after
  const char *types;
  IMP imp;
};

// IR: `{ ptr next, i32 count, [N x { ptr, ptr, ptr }] }`. `next` is null as emitted; the runtime
// chains category method lists onto it.
struct eden_objc_method_list {
  struct eden_objc_method_list *next;
  int count;
  struct eden_objc_method methods[1];   // [count]
};

// IR: `{ ptr name, ptr type, i32 offset }` — offset is superclass-relative, see note (2) above.
struct eden_objc_ivar {
  const char *name;
  const char *type;
  int offset;
};

// IR: `{ i32 count, [N x { ptr, ptr, i32 }] }` — note NO `next` pointer, unlike method lists.
struct eden_objc_ivar_list {
  int count;
  struct eden_objc_ivar ivars[1];       // [count]
};

// IR (18 fields): `{ ptr isa, ptr super_class, ptr name, i32 version, i32 info,
//                    i32 instance_size, ptr ivars, ptr methods, ptr dtable, ptr subclass_list,
//                    ptr sibling_class, ptr protocols, ptr gc_object_type, i32 abi_version,
//                    ptr ivar_offsets, ptr properties, i32 strong_pointers, i32 weak_pointers }`
// Observed constants: info = 17 (0x11) for classes, 18 (0x12) for metaclasses; abi_version = 1;
// metaclass instance_size = sizeof(this struct) as the compiler lays it out — 72 on wasm32, 144
// on LP64, 136 on LLP64. The trailing two words are ivar-layout bitmaps this runtime does not use
// (they matter only to a garbage collector) — they are declared so the struct's SIZE is right,
// which matters because arrays of classes are never indexed but the compiler's emitted
// initializer must line up field-for-field with what is read here.
struct eden_objc_class {
  struct eden_objc_class *isa;
  struct eden_objc_class *super_class;  // const char* name before resolution, Class after
  const char *name;
  long version;
  unsigned long info;
  long instance_size;                   // negative before resolution, see note (2)
  struct eden_objc_ivar_list *ivars;
  struct eden_objc_method_list *methods;
  void *dtable;
  struct eden_objc_class *subclass_list;
  struct eden_objc_class *sibling_class;
  void *protocols;
  void *gc_object_type;
  long abi_version;
  int **ivar_offsets;                   // parallel to ivars->ivars[], also superclass-relative
  void *properties;
  // POINTER-WIDTH, NOT `long` — measured, see the 64-bit block in this file's header. Declaring
  // these `long` is correct on wasm32 and LP64 by coincidence and wrong on Windows (LLP64),
  // where it makes this struct 128 bytes against the 136 clang emits. Unread GC bitmaps; only
  // the size matters, and __objc_exec_class() checks that size at startup.
  void *strong_pointers;
  void *weak_pointers;
};

// IR: `{ ptr category_name, ptr class_name, ptr instance_methods, ptr class_methods,
//        ptr protocols }`.
// Categories also carry the compiler's protocol-holder hack: every TU that declares a @protocol
// emits a fake category whose class_name is "__ObjC_Protocol_Holder_Ugly_Hack". It names no real
// class, so it is skipped naturally by the "class not found" path.
struct eden_objc_category {
  const char *category_name;
  const char *class_name;
  struct eden_objc_method_list *instance_methods;
  struct eden_objc_method_list *class_methods;
  void *protocols;
};

// IR: `{ ptr class_name, [N x ptr] instances }`, instances NULL-terminated, and the symtab holds
// a NULL-terminated array OF THESE lists.
//
// THIS IS THE `@"literal"` PATH — the 743 constant strings in the engine are emitted as static
// instances with a null `isa`, and this list is how the runtime learns to fill it in. Without
// this fixup every `@"..."` in the engine is an object with no class and the first message sent
// to one dereferences null. (See web-port-plan.md's "D3 refinement" for the layout those
// instances have and why NSConstantString must declare exactly three words.)
struct eden_objc_static_instances {
  const char *class_name;
  id instances[1];                      // NULL-terminated
};

// IR: `{ i32 sel_ref_cnt, ptr refs, i16 cls_def_cnt, i16 cat_def_cnt, [N x ptr] defs }`.
// `defs` holds cls_def_cnt classes, then cat_def_cnt categories, then a pointer to the static
// instances array (or null), then a null terminator.
struct eden_objc_symtab {
  unsigned long sel_ref_cnt;
  struct eden_objc_selector *refs;
  unsigned short cls_def_cnt;
  unsigned short cat_def_cnt;
  void *defs[1];
};

// IR: `{ i32 version, i32 size, ptr name, ptr symtab }` with version = 9 on every target and
// size = sizeof(this struct): 16 on wasm32, 32 on LP64, 24 on LLP64 (`unsigned long` is 4 bytes
// on Windows, so the two counts pack ahead of the pointers there).
struct eden_objc_module {
  unsigned long version;
  unsigned long size;
  const char *name;
  struct eden_objc_symtab *symtab;
};

// IR: the value `objc_msg_lookup_sender` returns; clang loads field index 4 as the IMP.
// Field names follow GNUstep's `struct objc_slot`. Only `method` is load-bearing here — the
// others exist so the struct is the right size and so a future inline-cache optimization has
// somewhere to put its bookkeeping (clang emits no cache checks at -O0..-O2, so nothing reads
// `version`/`cachedFor` today).
struct eden_objc_slot {
  struct eden_objc_class *owner;
  struct eden_objc_class *cachedFor;
  const char *types;
  int version;
  IMP method;
};

}  // extern "C"

#endif
