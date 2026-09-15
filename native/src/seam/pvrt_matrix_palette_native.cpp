// pvrt_matrix_palette_native.cpp — native replacement for Classes/PVRTglesExt.cpp
// (Phase N Stage 1, WORKING/native-migration-plan-2026-09-04.md). It provides the two functions
// that file defines — CPVRTglesExt::LoadExtensions() and ::IsGLExtensionSupported() — and, as
// part of LoadExtensions, installs the shim's GL_OES_matrix_palette entry points so creature
// models draw. Web achieves the same thing with `-Wl,--wrap=` over the real file
// (web/src/seam/pvrt_matrix_palette.cpp); read that file's header for the WHY, which is identical.
//
// TWO REASONS THIS IS A REPLACEMENT RATHER THAN A WRAP, and both are load-bearing:
//
//  1. **Apple's ld64 has no `--wrap`.** The plan flags this and says to decide per symbol rather
//     than as a blanket policy. For this symbol the answer is a replacement, because —
//  2. **the ORIGINAL file cannot compile on macOS at all.** PVRTglesExt.h picks
//     `#define PVRGetProcAddress(x) ::x` for `__APPLE__ && TARGET_OS_IPHONE`, and PVRTglesExt.cpp
//     then names ~50 ES1 extension entry points (`::glBindVertexArrayOES`, `::glClipPlanefIMG`,
//     ...) as real global symbols. On iOS those exist in the ES1 framework; on desktop GL they do
//     not exist at all, so the file is a wall of undeclared-identifier errors rather than a link
//     failure. The web build sidesteps it by not being `__APPLE__` (its macro expands to NULL);
//     native is `__APPLE__`, so `Classes/PVRTglesExt.cpp` goes in EDEN_NATIVE_SEAM_EXCLUDE and
//     this file stands in.
//
// The behavioural contract is the real file's, unchanged: LoadExtensions() clears the WHOLE
// function-pointer table first — the ~50 pointers this port does not emulate MUST stay null,
// because Model.mm calls through them with no null check and the extension probes are what keep
// it away from them — and only then are the four matrix-palette pointers overwritten.
#include "../../../Classes/PVRTglesExt.h"
#include "../../../web/src/shim/gl/gl_es1_shim.h"

#include <cstdio>
#include <cstring>

void CPVRTglesExt::LoadExtensions() {
    // The real implementation assigns 0 to every member individually (Classes/PVRTglesExt.cpp:57
    // onward). CPVRTglesExt is a plain aggregate of function pointers — no virtuals, no bases,
    // grep-verified — so one memset is the same thing, and it cannot fall out of date the way a
    // hand-maintained list of ~50 assignments would when the header gains a pointer.
    std::memset(this, 0, sizeof(*this));

    // Then the four the shim really implements (src/shim/gl/gl_fixed_function.cpp GROUP 8 — the
    // palette blend runs in a second, GLSL ES 3.00 / desktop-330 shader program because indexing
    // a uniform array by an attribute value is what ES 1.00 forbids).
    glCurrentPaletteMatrixOES =
        (PFNGLCURRENTPALETTEMATRIXOES)&eden_gl_glCurrentPaletteMatrixOES;
    glLoadPaletteFromModelViewMatrixOES =
        (PFNGLLOADPALETTEFROMMODELVIEWMATRIXOES)&eden_gl_glLoadPaletteFromModelViewMatrixOES;
    glMatrixIndexPointerOES =
        (PFNGLMATRIXINDEXPOINTEROES)&eden_gl_glMatrixIndexPointerOES;
    glWeightPointerOES =
        (PFNGLWEIGHTPOINTEROES)&eden_gl_glWeightPointerOES;

    // One line, once — same signal as the web twin. LoadExtensions() runs from LoadModels() only
    // after all seven POD files have been read successfully, so this printing at all is the
    // cheapest end-to-end confirmation that the extension gate opened AND the models are on disk
    // where the engine looks for them.
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::fprintf(stderr, "[eden-gl] matrix-palette entry points installed (creatures enabled).\n");
    }
}

// Transcribed from Classes/PVRTglesExt.cpp:275 — the standard "don't be fooled by sub-strings"
// GL_EXTENSIONS walk. Kept rather than simplified because Model.mm's LoadModels() gates the whole
// creature path on its answer, and the shim ADVERTISES GL_OES_matrix_palette in glGetString
// precisely so this returns true.
//
// One real difference from the original: a core-profile context can legally answer
// glGetString(GL_EXTENSIONS) with NULL (the enum was deprecated in favour of glGetStringi). The
// shim intercepts GL_EXTENSIONS and returns its own string, so that does not happen here — but
// the null guard stays, because the original would have strstr'd a null pointer and the failure
// would look like a crash inside model loading.
bool CPVRTglesExt::IsGLExtensionSupported(const char *extension) {
    if (!extension || *extension == '\0') return false;
    if (std::strchr(extension, ' ')) return false;   // extension names contain no spaces

    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (!extensions) return false;

    const char *start = extensions;
    for (;;) {
        const char *where = std::strstr(start, extension);
        if (!where) break;
        const char *terminator = where + std::strlen(extension);
        if (where == start || *(where - 1) == ' ') {
            if (*terminator == ' ' || *terminator == '\0') return true;
        }
        start = terminator;
    }
    return false;
}
