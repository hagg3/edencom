// objc_personality_native.cpp — the one Objective-C++ unwinder symbol clang references on ELF
// that this port's runtime does not otherwise provide. Phase N Stage 3.2.
//
// WHY IT APPEARS AT ALL, since this engine throws no Objective-C exceptions: clang gives every
// Objective-C++ translation unit that needs unwinding — i.e. every engine .mm with a C++ object
// that has a destructor — the OBJC++ personality routine rather than the C++ one, whether or not
// the file contains a single `@try`. So `Classes/Camera.mm` alone is enough to leave
// `__gnustep_objcxx_personality_v0` undefined at link time, and it does:
//
//     undefined reference to `__gnustep_objcxx_personality_v0'
//
// It does NOT appear on the other three targets, which is why it took until this stage to meet:
//   * Emscripten builds with exceptions off, so no personality is referenced at all;
//   * macOS uses Apple's runtime, whose personality libobjc supplies;
//   * Windows/MinGW uses SEH, and clang emits `__gxx_personality_seh0` there — a symbol
//     libunwind/libc++ already provides. So this file is ELF-only, deliberately.
//
// FORWARDING TO THE C++ PERSONALITY IS CORRECT HERE, NOT A STUB. GNUstep's own version differs
// from `__gxx_personality_v0` in exactly one respect: it can match an `@catch(SomeClass *)`
// against a thrown Objective-C object, using the runtime's class hierarchy. This engine throws no
// Objective-C exception and catches none — grep-verified across `Classes/`, `web/src/` and
// `native/src/`: not one `@throw`, `@try` or `@catch` anywhere, and the Foundation shim's own
// NSException aborts rather than unwinding (see NSErrorException.h). Every exception that can
// unwind through an engine frame is therefore a C++ one, and `__gxx_personality_v0` is the right
// and complete handler for it.
//
// IF THAT EVER STOPS BEING TRUE — if someone adds an `@throw` or an `@catch(NSString *)` — this
// forwarding does not silently do the wrong thing: the C++ personality will fail to match the
// Objective-C type and the exception will reach std::terminate, which is loud. The fix at that
// point is a real personality routine (and a real `objc_exception_throw`), not a wider stub.

#if !defined(__APPLE__) && !defined(_WIN32) && !defined(__EMSCRIPTEN__)

#include <unwind.h>

extern "C" {

_Unwind_Reason_Code __gxx_personality_v0(int version, _Unwind_Action actions,
                                         _Unwind_Exception_Class exceptionClass,
                                         _Unwind_Exception *exceptionObject,
                                         _Unwind_Context *context);

_Unwind_Reason_Code __gnustep_objcxx_personality_v0(int version, _Unwind_Action actions,
                                                    _Unwind_Exception_Class exceptionClass,
                                                    _Unwind_Exception *exceptionObject,
                                                    _Unwind_Context *context) {
  return __gxx_personality_v0(version, actions, exceptionClass, exceptionObject, context);
}

}  // extern "C"

#endif  // ELF, non-Apple, non-Emscripten
