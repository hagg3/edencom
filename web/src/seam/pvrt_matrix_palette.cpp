// pvrt_matrix_palette.cpp — installs the shim's GL_OES_matrix_palette entry points into
// CPVRTglesExt's function-pointer table (pass 27, creature skinning).
//
// WHY THIS EXISTS
// Classes/PVRTglesExt.cpp resolves every extension entry point through its own
// `PVRGetProcAddress(x)` macro, and on this platform that macro expands to a literal NULL:
// PVRTglesExt.h picks the `::x` form only for `__APPLE__ && TARGET_OS_IPHONE`, and emscripten is
// not `__APPLE__`, so the `EGL_NOT_PRESENT` branch (CMakeLists.txt defines it — see the comment
// there) wins. So even now that the shim ADVERTISES GL_OES_matrix_palette in glGetString
// (src/shim/gl/gl_es1_shim.cpp, GROUP 8) and LoadExtensions therefore enters the matrix-palette
// branch, all four pointers would be assigned NULL — and Classes/Model.mm:2717-2757 calls them
// through `m_Extensions.` with no null check, which on wasm is an immediate indirect-call trap.
//
// WHY A LINK WRAP AND NOT A WRITE TO m_Extensions
// `m_Extensions` IS a plain global in Model.mm (not file-static), so it could be written from
// here — but only AFTER LoadModels() has run, and LoadModels() calls LoadExtensions() and then
// immediately uses the models. There is no moment in between that a seam can hook. Wrapping
// LoadExtensions() itself puts the fix-up exactly where the nulls are written. The wrap works
// because the call crosses a translation unit (defined in PVRTglesExt.cpp, called from Model.mm);
// wasm-ld's --wrap never sees intra-TU calls. Same lever as the retired
// model_render_guard.cpp — see CMakeLists.txt's link-options block.
//
// The real LoadExtensions() is still called first: it clears the whole table (including the ~50
// pointers this port does not emulate, which MUST stay NULL) and does the extension-string
// probes. Only the four matrix-palette pointers are then overwritten.
#include "../../../Classes/PVRTglesExt.h"
#include "../shim/gl/gl_es1_shim.h"
#include <cstdio>

extern "C" {

// `this` is an ordinary leading pointer parameter in the Itanium C++ ABI clang uses for wasm, so
// the wrapper can be plain C. The mangled name was read off the real object file
// (`llvm-nm build-st/.../PVRTglesExt.cpp.o`), not guessed.
void __real__ZN12CPVRTglesExt14LoadExtensionsEv(CPVRTglesExt* self);

void __wrap__ZN12CPVRTglesExt14LoadExtensionsEv(CPVRTglesExt* self) {
    __real__ZN12CPVRTglesExt14LoadExtensionsEv(self);
    if (!self) return;
    self->glCurrentPaletteMatrixOES =
        (CPVRTglesExt::PFNGLCURRENTPALETTEMATRIXOES)&eden_gl_glCurrentPaletteMatrixOES;
    self->glLoadPaletteFromModelViewMatrixOES =
        (CPVRTglesExt::PFNGLLOADPALETTEFROMMODELVIEWMATRIXOES)
            &eden_gl_glLoadPaletteFromModelViewMatrixOES;
    self->glMatrixIndexPointerOES =
        (CPVRTglesExt::PFNGLMATRIXINDEXPOINTEROES)&eden_gl_glMatrixIndexPointerOES;
    self->glWeightPointerOES =
        (CPVRTglesExt::PFNGLWEIGHTPOINTEROES)&eden_gl_glWeightPointerOES;
    // One line, once. LoadExtensions() runs from LoadModels() only AFTER all seven POD files have
    // been read successfully, so this printing at all is the cheapest end-to-end signal that the
    // extension gate opened and the models are on disk where the engine looks for them.
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::fprintf(stderr, "[eden-gl] matrix-palette entry points installed (creatures enabled).\n");
    }
}

}  // extern "C"
