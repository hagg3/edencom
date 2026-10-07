// TextRaster_native.cpp — native implementation of `eden_rasterize_text_rgba`, the one genuinely
// platform-shaped function in web/src/seam/Texture2D_web.mm (Phase N Stage 1,
// WORKING/native-migration-plan-2026-09-04.md).
//
// Everything else in Texture2D_web.mm is shared verbatim between the two targets — the PNG decode
// (stb_image), the POT rounding, the pixel-format selection, the initData contract. Only the
// string-to-pixels step differed: web has a real DOM and rasterizes through a 2D canvas context,
// and there is no canvas here. `stb_truetype.h` was already vendored at web/src/shim/vendor/ for
// exactly this eventuality (the plan names it as native's text-raster answer).
//
// THE CONTRACT THIS MUST HONOUR, copied from the web twin's header comment because getting either
// half wrong is invisible until something looks subtly wrong on screen:
//   * `outPtr` is width*height*4 bytes, caller-owned and PRE-ZEROED. Doing nothing is therefore a
//     legal outcome — it degrades to a transparent texture rather than to garbage, which is what
//     "no font available" must look like.
//   * Row 0 is the TOP row. This port has a no-V-flip convention throughout (GL's V=0 is the
//     image's top row everywhere here); stb_truetype's bitmaps are already top-down, so like the
//     canvas path and unlike the original CGContext path, no flip is applied.
//   * White (255,255,255) premultiplied by nothing — the glyph coverage goes into ALL FOUR
//     channels' alpha slot only, i.e. RGB stays white and A carries coverage. The engine tints
//     these textures with the current colour, so a coloured raster here would multiply twice.
//
// Real call sites are the same as on web (they are the reason this is not dead code):
// statusbar.mm's `new Texture2D(status, ...)` draws the world-name label under the menu's world
// picker, and SharedList.mm draws world/date labels.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#if defined(EDEN_PLATFORM_IOS)
#include <dirent.h>     // the system-font directory scan below
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

extern "C" const char* eden_platform_bundle_root(void);   // web/src/shim/foundation/platform_shims.cpp

namespace {

// One font, loaded once, kept for the process lifetime — these rasters happen at UI-build time,
// not per frame, but re-reading a 20 MB system font per label would still be silly.
//
// Candidates in preference order. A .ttc (font collection) is handled by asking stb_truetype for
// face 0's offset, which is what stbtt_GetFontOffsetForIndex exists for — without that call the
// header parse silently produces a font with no glyphs, which is the failure mode that looks like
// "text renders as nothing" rather than like an error.
const char* kFontCandidates[] = {
#if defined(EDEN_PLATFORM_IOS)
    // iOS (Phase N Stage 4.1). The device's fonts live under /System/Library/Fonts/, which a
    // sandboxed app CAN read — but the file NAMES there are not API and have changed across
    // releases (Core/ vs Cache/ vs AppFonts/, .ttc collections that come and go), and this stage
    // targets iOS 12 through 26. So the list below is a best guess and the DIRECTORY SCAN in
    // Font::Font() is the real answer; the named entries only exist to skip the scan on the
    // common case.
    //
    // The alternative — CoreText, which would answer authoritatively — was rejected for the same
    // reason the CGImage layer stays this port's own (native/CMakeLists.txt's header): pulling
    // <CoreText/CoreText.h> into this translation unit drags CoreGraphics in behind it, and this
    // file is compiled with the port's shadowing framework/ directory on its include path.
    "/System/Library/Fonts/Core/Helvetica.ttc",
    "/System/Library/Fonts/Core/HelveticaNeue.ttc",
    "/System/Library/Fonts/Cache/Helvetica.ttc",
    "/System/Library/Fonts/CoreAddition/ArialHB.ttc",
#elif defined(__APPLE__)
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/SFNSMono.ttf",
    "/Library/Fonts/Arial.ttf",
#elif defined(_WIN32)
    // Phase N Stage 3. %WINDIR% rather than a literal C:\Windows — the system drive is not
    // guaranteed to be C:, and getenv is cheaper than pulling in <windows.h> for
    // GetWindowsDirectory in a file that otherwise needs nothing from it. Arial first because it
    // is what the iOS original used; Segoe UI is the shipped default on every supported Windows
    // and Tahoma is the fallback that predates it.
    "%WINDIR%/Fonts/arial.ttf",
    "%WINDIR%/Fonts/segoeui.ttf",
    "%WINDIR%/Fonts/tahoma.ttf",
    "C:/Windows/Fonts/arial.ttf",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
#endif
};

struct Font {
    std::vector<unsigned char> bytes;
    stbtt_fontinfo info;
    bool ok = false;

    Font() {
        for (const char* candidate : kFontCandidates) {
            // "%WINDIR%/..." -> the real directory. One variable, expanded only when the entry
            // starts with it, so every other candidate is still a plain literal.
            std::string path = candidate;
            if (path.compare(0, 8, "%WINDIR%") == 0) {
                const char* win = getenv("WINDIR");
                if (!win || !*win) continue;
                path = std::string(win) + path.substr(8);
            }
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (len <= 0) { fclose(f); continue; }
            bytes.resize((size_t)len);
            size_t got = fread(bytes.data(), 1, (size_t)len, f);
            fclose(f);
            if (got != (size_t)len) { bytes.clear(); continue; }
            int offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
            if (offset < 0) { bytes.clear(); continue; }
            if (!stbtt_InitFont(&info, bytes.data(), offset)) { bytes.clear(); continue; }
            ok = true;
            return;
        }
#if defined(EDEN_PLATFORM_IOS)
        // THE SCAN, and it is the iOS path's primary mechanism rather than its fallback (see the
        // candidate list). Take the first .ttf/.ttc in any of the three directories iOS has ever
        // kept its system faces in: which face it is does not matter here — every one of them
        // covers ASCII, and this rasteriser only ever draws world names and dates.
        //
        // Same failure shape as Stage 3's Windows leg, which shipped with an empty candidate list
        // and rendered every label blank without erroring. A scan cannot have that bug.
        for (const char* dir : { "/System/Library/Fonts/Core", "/System/Library/Fonts/Cache",
                                 "/System/Library/Fonts", "/System/Library/Fonts/AppFonts" }) {
            DIR* d = opendir(dir);
            if (!d) continue;
            while (struct dirent* e = readdir(d)) {
                const char* dot = std::strrchr(e->d_name, '.');
                if (!dot) continue;
                if (std::strcmp(dot, ".ttf") != 0 && std::strcmp(dot, ".ttc") != 0 &&
                    std::strcmp(dot, ".otf") != 0) continue;
                std::string path = std::string(dir) + "/" + e->d_name;
                FILE* f = fopen(path.c_str(), "rb");
                if (!f) continue;
                fseek(f, 0, SEEK_END);
                long len = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (len <= 0) { fclose(f); continue; }
                bytes.resize((size_t)len);
                size_t got = fread(bytes.data(), 1, (size_t)len, f);
                fclose(f);
                if (got != (size_t)len) { bytes.clear(); continue; }
                int offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
                if (offset < 0) { bytes.clear(); continue; }
                if (!stbtt_InitFont(&info, bytes.data(), offset)) { bytes.clear(); continue; }
                std::fprintf(stderr, "[eden-text] using scanned system font %s\n", path.c_str());
                ok = true;
                closedir(d);
                return;
            }
            closedir(d);
        }
#endif
        // Not fatal, and deliberately loud exactly once: a build with no system font still runs,
        // it just draws blank labels, and silence there would be indistinguishable from a
        // layout bug.
        std::fprintf(stderr, "[eden-text] no usable system font found — string textures will be "
                             "blank (see TextRaster_native.cpp's candidate list).\n");
    }
};

Font& font() {
    static Font f;
    return f;
}

// The design system's DISPLAY face (web/docs/design-system.md, "Type"): Jersey 10, SIL OFL, the
// same file the web UI self-hosts (web/public/assets/fonts/), linked into the flat bundle by
// native/CMakeLists.txt. Only GL-kit chrome asks for it (eden_text_raster_set_face below); a
// missing file falls back to the body face rather than to blank labels.
struct DisplayFont {
    std::vector<unsigned char> bytes;
    stbtt_fontinfo info;
    bool ok = false;

    DisplayFont() {
        const std::string path = std::string(eden_platform_bundle_root()) + "/Jersey10-Regular.ttf";
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) {
            std::fprintf(stderr, "[eden-text] display face missing (%s) — chrome falls back to "
                                 "the body face.\n", path.c_str());
            return;
        }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len > 0) {
            bytes.resize((size_t)len);
            if (fread(bytes.data(), 1, (size_t)len, f) == (size_t)len) {
                int offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
                ok = offset >= 0 && stbtt_InitFont(&info, bytes.data(), offset);
            }
        }
        fclose(f);
    }
};

int g_face = 0;   // 0 = body (system sans), 1 = display (Jersey 10)

const stbtt_fontinfo* active_font() {
    if (g_face == 1) {
        static DisplayFont d;
        if (d.ok) return &d.info;
    }
    Font& f = font();
    return f.ok ? &f.info : nullptr;
}

}  // namespace

// Selects the face the NEXT rasters use. Sticky, so a caller sets it, builds, and puts it back —
// GLW::Label does exactly that. Every other caller (statusbar.mm, SharedList.mm) never calls this
// and keeps the body face.
extern "C" void eden_text_raster_set_face(int face) { g_face = (face == 1) ? 1 : 0; }

extern "C" void eden_rasterize_text_rgba(const char* textC, int width, int height, float fontPx,
                                         int align, unsigned char* outPtr) {
    if (!textC || !outPtr || width <= 0 || height <= 0) return;
    const stbtt_fontinfo* fi = active_font();
    if (!fi) return;   // pre-zeroed buffer stands — see the contract note above

    const float scale = stbtt_ScaleForPixelHeight(fi, fontPx);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(fi, &ascent, &descent, &lineGap);

    // Measure first, so alignment can be applied. ASCII only, matching what the engine actually
    // puts through here (world names and dates); a multi-byte UTF-8 sequence degrades to its
    // individual bytes rather than to a crash.
    float advance = 0.0f;
    for (const char* p = textC; *p; ++p) {
        int aw = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(fi, (unsigned char)*p, &aw, &lsb);
        advance += aw * scale;
        if (p[1]) advance += stbtt_GetCodepointKernAdvance(fi,
                                                           (unsigned char)p[0],
                                                           (unsigned char)p[1]) * scale;
    }

    // Same three cases and the same anchor points as the canvas path: left at x=0, centre at
    // width/2, right at width; baseline placed so the text is vertically centred, which is what
    // `ctx.textBaseline = 'middle'` means.
    float x;
    if (align == 1)      x = (width - advance) * 0.5f;
    else if (align == 2) x = (float)width - advance;
    else                 x = 0.0f;

    const float baseline = height * 0.5f + (ascent + descent) * 0.5f * scale;

    for (const char* p = textC; *p; ++p) {
        const int cp = (unsigned char)*p;
        int gw = 0, gh = 0, gxo = 0, gyo = 0;
        unsigned char* glyph = stbtt_GetCodepointBitmap(fi, scale, scale, cp,
                                                        &gw, &gh, &gxo, &gyo);
        if (glyph) {
            const int gx = (int)(x + 0.5f) + gxo;
            const int gy = (int)(baseline + 0.5f) + gyo;
            for (int row = 0; row < gh; ++row) {
                const int dy = gy + row;
                if (dy < 0 || dy >= height) continue;
                for (int col = 0; col < gw; ++col) {
                    const int dx = gx + col;
                    if (dx < 0 || dx >= width) continue;
                    const unsigned char cov = glyph[row * gw + col];
                    if (!cov) continue;
                    unsigned char* px = outPtr + ((size_t)dy * width + dx) * 4;
                    // Max-blend rather than overwrite so overlapping glyph boxes (kerned pairs,
                    // italics) do not punch holes in each other's coverage.
                    px[0] = 255; px[1] = 255; px[2] = 255;
                    if (cov > px[3]) px[3] = cov;
                }
            }
            stbtt_FreeBitmap(glyph, nullptr);
        }
        int aw = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(fi, cp, &aw, &lsb);
        x += aw * scale;
        if (p[1]) x += stbtt_GetCodepointKernAdvance(fi, cp, (unsigned char)p[1]) * scale;
    }
}
