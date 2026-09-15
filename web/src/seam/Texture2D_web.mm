// Texture2D_web.mm — Stage P2 seam replacement for Classes/Texture2D.mm.
//
// WHY THIS FILE REUSES THE ORIGINAL HEADER: Classes/Texture2D.h is `#import "Texture2D.h"`'d
// (quoted) by 8 non-seam ENGINE headers this port must not edit (Graphics.h, Menu.h, Resources.h,
// SettingsMenu.h, Util.h, SharedList.h, Terrain.h, World.h — grep-verified pass 2). Quoted
// includes resolve relative to the including file first, so those headers always see the real,
// untouched Classes/Texture2D.h. This file supplies a NEW @implementation-equivalent (Texture2D
// is a plain C++ class, not Objective-C) of that SAME interface — same pattern as
// EAGLView_web.mm's header comment describes.
//
// WHAT THIS FILE OWES, measured against the real 1555-line Texture2D.mm (archive/PORT-STATUS-2026-08-13.md "Pass
// 10"): Texture2D is the engine's immediate-mode quad drawer for ALL 2D/UI/sky — every menu
// button, HUD icon, and the sky backdrop draws through one of the `draw*` methods below. Two
// clean halves:
//   1. THE DRAW METHODS are pure GL (client arrays + glBindTexture + glDrawArrays) and are near-
//      verbatim ports — the ES1 state-pairing risk that made Stage P2 "Opus" lived in the GL shim
//      (gl_es1_shim.cpp GROUP 2d), not here, and that shim is done and browser-verified.
//   2. THE PIXEL SOURCE is the real work this file does: initFromPath decodes PNGs via stb_image
//      instead of UIImage/CGBitmapContext, replicating initFromImage's pow-of-two padding, the
//      upside-down-texture flip (Texture2D.h's own doc comment: "content will be upside-down"),
//      and the per-pixel-format packing (RGB565/RGBA4444/RGBA5551/RGB888) bit-for-bit.
//
// THE RECOLOR PIPELINE — implemented 2026-07-31 (project audit row 11 / A5), was a stub before.
// initFromImage(CGImageRef, ...) / ManipulateImagePixelData(2) / the storeImage bookkeeping are
// all live. This comment block used to say they were "DELIBERATELY NOT PORTED (P2b, later)" on
// the reasoning that the only call sites were creature skins, gated behind Model.mm's CPU-skinning
// rewrite. That reasoning was wrong on the facts: three of the four call sites have nothing to do
// with creatures — Hud.mm's paint icon and painted build icons, and Terrain.mm's doors — and all
// three were drawing NOTHING as a result. Worth keeping as a lesson: "this path is only reached by
// X" deserves a grep, not a recollection. Full write-up in web/docs/resources-and-audio.md; the
// standing regression guard is tools/headless-recolor-test.js.
//
// DELIBERATELY NOT PORTED:
//   - initFromString / the `(NSString*, CGSize, UITextAlignment, UIFont*)` text-rasterizing
//     constructor. CORRECTION (see archive/PORT-STATUS-2026-08-13.md "Owed/open" + RESUME-HERE pass 35): the old
//     claim here — "confirmed dead code, only call site is the commented-out Graphics::drawText"
//     — was wrong. statusbar.mm's `setStatus` calls `new Texture2D(status, ...)` directly (the
//     world-name label under the menu's world picker, and SharedList.mm's world/date labels), and
//     that path is very much live. Implemented below via an HTML canvas 2D context (EM_JS) rather
//     than stb_truetype, since Emscripten gives this port a real DOM to rasterize with.
//     (This entry was itself a correction of an earlier wrong "dead code" claim — the same
//     mistake the recolor block above records. Two for two on that pattern in one file.)

// printg is normally supplied by the force-included Eden_Prefix.pch (engine .mm sources only,
// per CMakeLists.txt's COMPILE_OPTIONS) — this seam file isn't in that list, so it needs its own
// definition before OpenGL_Internal.h's REPORT_ERROR/CHECK_GL_ERROR macros can use it. Matches
// the pch's __DEBUG__ branch (`printg(...) printf(__VA_ARGS__)`) unconditionally.
#define printg(...) printf(__VA_ARGS__)

#import <OpenGLES/ES1/glext.h>   // matches Texture2D.mm's own top-of-file include
// The PVRTC compressed-format tokens are deliberately NOT in this port's glext.h trampoline
// (see src/shim/gl/framework/OpenGLES/ES1/glext.h's header comment — no engine, non-PVRT file
// was believed to need them). initData()'s switch statement below still needs them to name the
// cases even though this port never actually feeds it PVRTC data (assets are plain PNGs decoded
// by stb_image) — Khronos-canonical values, so nothing to renumber if PVRTC decode is ever added.
#ifndef GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG
#define GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG  0x8C00
#define GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG  0x8C01
#define GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG 0x8C02
#define GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG 0x8C03
#endif
#import "../../../Classes/Texture2D.h"
#import "../../../Classes/OpenGL_Internal.h"
#import "../../../Classes/Globals.h"
#import "../../../Classes/World.h"
#import "../shim/foundation/uikit_stubs.h"
#include "../shim/foundation/NSBundle.h"
#include "../shim/foundation/NSFileManager.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO   // we read the file ourselves (Emscripten preload FS is real POSIX, but
                         // going through NSData keeps one file-reading code path in the port)
#include "stb_image.h"

#include "../shim/foundation/platform_shims.h"   // EDEN_EXPORT (Phase N Stage 1)

#include <cstdlib>
#include <cstring>
#include <algorithm>

// Texture2D.mm's kMaxTextureSize is a private #define local to that .mm, not in the header —
// redeclared here under a distinct name to avoid any accidental collision if that header ever
// changes.
static const int kMaxTextureSize_Eden = 1024;

// =============================================================================================
// initData — UNCHANGED FROM THE ENGINE. This constructor pair takes already-decoded raw pixels
// and is inline in the unmodified Texture2D.h... except initData() itself (the actual GL upload)
// is defined in the .mm, so it is ported here VERBATIM (byte-for-byte identical to
// Classes/Texture2D.mm's initData) — pure GL, no CoreGraphics, nothing to translate.
// =============================================================================================
void Texture2D::initData(const void* data, Texture2DPixelFormat pixelFormat, int width, int height,
                          CGSize size, BOOL genMips) {
    GLint saveName;

    glGenTextures(1, &name);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saveName);
    glBindTexture(GL_TEXTURE_2D, name);
    if (genMips) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    switch (pixelFormat) {
        case kTexture2DPixelFormat_RGBA8888:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
            break;
        case kTexture2DPixelFormat_RGBA4444:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, data);
            break;
        case kTexture2DPixelFormat_RGBA5551:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, data);
            break;
        case kTexture2DPixelFormat_RGB565:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, data);
            break;
        case kTexture2DPixelFormat_RGB888:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
            break;
        case kTexture2DPixelFormat_L8:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, data);
            break;
        case kTexture2DPixelFormat_A8:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, width, height, 0, GL_ALPHA, GL_UNSIGNED_BYTE, data);
            break;
        case kTexture2DPixelFormat_LA88:
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, width, height, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, data);
            break;
        case kTexture2DPixelFormat_RGB_PVRTC2:
            glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG, width, height, 0, (width * height) / 4, data);
            break;
        case kTexture2DPixelFormat_RGB_PVRTC4:
            glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG, width, height, 0, (width * height) / 2, data);
            break;
        case kTexture2DPixelFormat_RGBA_PVRTC2:
            glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG, width, height, 0, (width * height) / 4, data);
            break;
        case kTexture2DPixelFormat_RGBA_PVRTC4:
            glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, width, height, 0, (width * height) / 2, data);
            break;
        default:
            [NSException raise:NSInternalInconsistencyException format:@""];
    }
    glBindTexture(GL_TEXTURE_2D, saveName);

    if (!CHECK_GL_ERROR()) {
        printf("err initing texture\n");
        return;
    }

    _size = size;
    _width = width;
    _height = height;
    _format = pixelFormat;
    _maxS = size.width / (float)width;
    _maxT = size.height / (float)height;
}

Texture2D::Texture2D(const void* data, Texture2DPixelFormat pixelFormat, int width, int height, CGSize size) {
    initData(data, pixelFormat, width, height, size, FALSE);
}
Texture2D::Texture2D(const void* data, Texture2DPixelFormat pixelFormat, int width, int height, CGSize size, BOOL genMips) {
    initData(data, pixelFormat, width, height, size, genMips);
}

Texture2D::~Texture2D() {
    if (name) glDeleteTextures(1, &name);
}

NSString* Texture2D::description() {
    return [NSString stringWithFormat:@"< Texture2D| Name = %i | Dimensions = %ix%i | Coordinates = (%.2f, %.2f)>",
            name, (int)_width, (int)_height, (double)_maxS, (double)_maxT];
}

// =============================================================================================
// PNG decode (stb_image) + the pow-of-two/flip/format-pack pipeline, replacing initFromImage's
// CGBitmapContext version. Ported for behavior, not byte-for-byte, because CGBitmapContext's
// premultiplied-ARGB-big-endian dance is a CoreGraphics implementation detail our stb_image
// buffer (tightly-packed RGBA8, top-left origin) never has in the first place.
// =============================================================================================
namespace {

// Loads `path` via stb_image, forcing 4 channels (RGBA8) so every downstream format-pack case
// has one uniform source layout. `outHasAlpha` reports the image's REAL channel count (stb still
// tells us via `comp` even when we force 4) — needed to reproduce initFromImage's "Automatic"
// pixel-format choice (RGBA8888 if the source had alpha, RGB565 if not).
unsigned char* EdenLoadPNG(NSString* path, int* outW, int* outH, BOOL* outHasAlpha) {
    NSData* fileData = [NSData dataWithContentsOfFile:path];
    if (!fileData || [fileData length] == 0) {
        // Pass 12/13: this branch previously had NO diagnostic, unlike the stb-decode-failure
        // branch below — and a browser run showed draws succeeding (glErr=0, textured=N/N) with
        // a still-black canvas. Prime suspect: this branch is firing silently (path resolution
        // wrong, or the preloaded FS doesn't contain what NSBundle's index thinks it does), name
        // stays 0, every draw binds "no texture", and WebGL samples an unbound/incomplete
        // texture as opaque black (spec-defined) — indistinguishable from the black clear color.
        // The "textured=N/N" debug counter (gl_es1_shim.cpp) only checks GL_TEXTURE_2D-enabled +
        // texcoords-enabled, NOT that the bound texture name is nonzero — so it cannot catch
        // this. TODO next session: if this prints, the bug is path resolution
        // (NSBundle.mm/CMakeLists.txt preload paths); if it does NOT print, decode is reaching
        // stb_image and the black canvas has a different cause (see STATUS.md).
        printg("Texture2D_web: no data at '%s' (NSData load failed or empty)\n", [path UTF8String]);
        return nullptr;
    }
    int comp = 0;
    unsigned char* pixels = stbi_load_from_memory(
        (const unsigned char*)[fileData bytes], (int)[fileData length], outW, outH, &comp, 4);
    if (!pixels) {
        printg("Texture2D_web: stb_image failed to decode '%s': %s\n",
               [path UTF8String], stbi_failure_reason());
        return nullptr;
    }
    if (outHasAlpha) *outHasAlpha = (comp == 4 || comp == 2) ? YES : NO;
    return pixels;
}

// Same power-of-two rounding rule as the engine's own (Texture2D.mm initFromImage): a dimension
// that is ALREADY a power of two (or exactly 1) is left untouched — only a non-power-of-two
// dimension gets rounded, and sizeToFit changes the rounding target from "next pow2 >= v" to
// "next pow2 such that 2x it >= v" (i.e. one power looser, so scaling up to fill loses less).
int EdenRoundDimension(int v, BOOL sizeToFit) {
    if (v == 1 || (v & (v - 1)) == 0) return v; // already a power of two (or 1): unchanged
    int i = 1;
    while ((sizeToFit ? 2 * i : i) < v) i *= 2;
    return i;
}

}  // namespace

// Diagnostic-only: the per-texture decode line below is far more useful with a filename on it,
// but initFromImage has no path (its other caller is the recolor pipeline, which has no file at
// all). initFromPath parks the name it is working on here for the duration of the call. Not
// thread-safe and does not need to be — CLAUDE.md convention #4, all of this runs on the one
// thread that owns GL.
static const char* g_decodeLabel = "<CGImage>";

// Real since audit row 11 (A5) — this used to be an empty stub, and everything downstream of it
// (paint icon, painted build icons, doors, creature skins) drew nothing as a result.
//
// This now carries the whole POT-rounding / canvas-placement / pixel-format-packing pipeline that
// used to live inline in initFromPath, because that is the engine's own shape: Texture2D.mm's
// initFromPath decodes to a UIImage and then calls initFromImage([uiImage CGImage], …). Both of
// this port's sources — a PNG off the preloaded FS and a freshly recolored buffer out of
// ManipulateImagePixelData — arrive here as a CGImage and take exactly one code path.
//
// `orientation` is accepted and ignored, as it was before: every call site in this engine passes
// [uiImage imageOrientation], every image the port produces is UIImageOrientationUp, and the real
// engine's own initFromImage only consults it to build a CGAffineTransform it then applies to a
// CGContext this port does not have.
void Texture2D::initFromImage(CGImageRef image, UIImageOrientation orientation, BOOL sizeToFit,
                               Texture2DPixelFormat pixelFormat, BOOL genMips) {
    (void)orientation;
    if (!image || !image->rgba) {
        // Matches the engine's own silent-failure shape (Texture2D.mm initFromImage's
        // `if(image == NULL) return;`) — `name` stays 0, which every draw call already treats as
        // "nothing to bind" via the destructor's `if(name)` guard.
        return;
    }
    const unsigned char* src = image->rgba;
    const int srcW = image->width;
    const int srcH = image->height;

    if (pixelFormat == kTexture2DPixelFormat_Automatic) {
        pixelFormat = image->hasAlpha ? kTexture2DPixelFormat_RGBA8888 : kTexture2DPixelFormat_RGB565;
    }

    CGSize imageSize = CGSizeMake((float)srcW, (float)srcH);
    int width = EdenRoundDimension(srcW, sizeToFit);
    int height = EdenRoundDimension(srcH, sizeToFit);
    // Oversized cap: proportional halving (not a hard clamp to kMaxTextureSize_Eden), matching
    // Texture2D.mm's `while((width>kMax)||(height>kMax)){width/=2;height/=2;...}` — rare in
    // practice (every current UI/atlas asset is well under 1024) but kept for fidelity.
    while (width > kMaxTextureSize_Eden || height > kMaxTextureSize_Eden) {
        width /= 2;
        height /= 2;
    }

    // Build the RGBA8 canvas at (width, height): sizeToFit scales the source to fill it exactly
    // (nearest-neighbour — the engine's own GL_NEAREST_MIPMAP_LINEAR filtering dominates visually
    // over the resample method, and every sizeToFit=TRUE caller in this engine is UI/icon art at
    // sizes where the difference is not visible); otherwise the source is placed at the top-left
    // with the rest left transparent, matching CGContextClearRect + CGContextTranslateCTM(0,
    // height-imageSize.height) in the original — net effect: image anchored to the texture's
    // TOP edge, remaining rows below it clear.
    //
    // Rows are copied in SOURCE ORDER (no V flip). initFromImage in the original (Texture2D.mm
    // :970-979) does NOT flip: it only translates to anchor at the top, so bitmap row 0 is the
    // image's top row and GL's V=0 lands on the image top. That IS the "upside-down" convention
    // Texture2D.h documents, and every texcoord in the engine is authored against it. The only
    // flipped CTM in the original is in the TEXT path (Texture2D.mm:1111), where the comment
    // spells out that it exists solely because NSString draws in the UIKit referential.
    // This used to flip (dstY = height-1-y), which inverted the 32-tile block atlas through the
    // glScalef(1,1/32,1) texture matrix in Terrain.mm:2597 — tile row r sampled row 31-r, so every
    // block drew a coherent but wrong texture (colors were unaffected, which masked the cause).
    unsigned char* canvas = (unsigned char*)calloc((size_t)width * height * 4, 1);
    for (int y = 0; y < height; ++y) {
        int srcY;
        if (sizeToFit) {
            srcY = (int)((float)y * srcH / (float)height);
            if (srcY >= srcH) srcY = srcH - 1;
        } else {
            srcY = y; // top-left anchor; rows beyond srcH stay transparent (calloc'd)
            if (srcY >= srcH) continue;
        }
        unsigned char* dstRow = canvas + (size_t)y * width * 4;
        for (int x = 0; x < width; ++x) {
            int srcX;
            if (sizeToFit) {
                srcX = (int)((float)x * srcW / (float)width);
                if (srcX >= srcW) srcX = srcW - 1;
            } else {
                srcX = x;
                if (srcX >= srcW) { continue; }
            }
            const unsigned char* s = src + ((size_t)srcY * srcW + srcX) * 4;
            unsigned char* d = dstRow + (size_t)x * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }

    // Per-pixel-format packing — bit patterns copied verbatim from Texture2D.mm's initFromImage
    // (RGBA8888 source assumed there too, just produced by CGBitmapContext instead of stb_image).
    void* finalData = canvas;
    void* toFree = canvas;
    size_t pixCount = (size_t)width * height;
    if (pixelFormat == kTexture2DPixelFormat_RGB888) {
        unsigned char* out = (unsigned char*)malloc(pixCount * 3);
        const unsigned char* in = canvas;
        for (size_t i = 0; i < pixCount; ++i) { out[i*3]=in[i*4]; out[i*3+1]=in[i*4+1]; out[i*3+2]=in[i*4+2]; }
        finalData = out;
    } else if (pixelFormat == kTexture2DPixelFormat_RGB565) {
        unsigned short* out = (unsigned short*)malloc(pixCount * 2);
        const unsigned char* in = canvas;
        for (size_t i = 0; i < pixCount; ++i) {
            unsigned char r = in[i*4], g = in[i*4+1], b = in[i*4+2];
            out[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        }
        finalData = out;
    } else if (pixelFormat == kTexture2DPixelFormat_RGBA4444) {
        unsigned short* out = (unsigned short*)malloc(pixCount * 2);
        const unsigned char* in = canvas;
        for (size_t i = 0; i < pixCount; ++i) {
            unsigned char r = in[i*4], g = in[i*4+1], b = in[i*4+2], a = in[i*4+3];
            out[i] = ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4);
        }
        finalData = out;
    } else if (pixelFormat == kTexture2DPixelFormat_RGBA5551) {
        unsigned short* out = (unsigned short*)malloc(pixCount * 2);
        const unsigned char* in = canvas;
        for (size_t i = 0; i < pixCount; ++i) {
            unsigned char r = in[i*4], g = in[i*4+1], b = in[i*4+2], a = in[i*4+3];
            out[i] = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7);
        }
        finalData = out;
    } else if (pixelFormat == kTexture2DPixelFormat_LA88) {
        unsigned char* out = (unsigned char*)malloc(pixCount * 2);
        const unsigned char* in = canvas;
        for (size_t i = 0; i < pixCount; ++i) { out[i*2]=in[i*4]; out[i*2+1]=in[i*4+3]; }
        finalData = out;
    } else if (pixelFormat == kTexture2DPixelFormat_L8) {
        unsigned char* out = (unsigned char*)malloc(pixCount);
        const unsigned char* in = canvas;
        for (size_t i = 0; i < pixCount; ++i) out[i] = in[i*4];
        finalData = out;
    } else if (pixelFormat == kTexture2DPixelFormat_A8) {
        unsigned char* out = (unsigned char*)malloc(pixCount);
        const unsigned char* in = canvas;
        for (size_t i = 0; i < pixCount; ++i) out[i] = in[i*4+3];
        finalData = out;
    }
    // else RGBA8888: canvas is already exactly that layout, used as-is.

    // Pass 13: one-time-per-texture diagnostic (not per-frame — ~120 assets total, cheap even
    // under printg's stdio path). A browser run showed draws succeeding (glErr=0, textured=N/N)
    // with a still-black canvas, and Pass 13's other diagnostics ruled out "no data reached
    // stb_image" for the menu-critical assets. This is the next rung down the stack: confirm the
    // DECODED pixels are plausible (nonzero, not uniformly transparent) before suspecting GL
    // state. `canvas` (pre-format-pack, always RGBA8) is sampled at its center so padding at the
    // edges (non-sizeToFit loads narrower than their pow2 canvas) can't produce a false negative.
    {
        size_t cx = (size_t)(width / 2), cy = (size_t)(height / 2);
        const unsigned char* p = canvas + (cy * width + cx) * 4;
        printg("Texture2D_web: '%s' decoded %dx%d (canvas %dx%d) fmt=%d center-rgba=(%d,%d,%d,%d)\n",
               g_decodeLabel, srcW, srcH, width, height, (int)pixelFormat, p[0], p[1], p[2], p[3]);
    }

    initData(finalData, pixelFormat, width, height, imageSize, genMips);

    if (finalData != toFree) free(finalData);
    free(toFree);
}
Texture2D::Texture2D(CGImageRef image, UIImageOrientation orientation, BOOL sizeToFit,
                     Texture2DPixelFormat pixelFormat, BOOL genMips) {
    initFromImage(image, orientation, sizeToFit, pixelFormat, genMips);
}

// Rasterizes `textC` into an RGBA8 buffer at `outPtr` (width*height*4 bytes, caller-owned,
// pre-zeroed so a headless/no-canvas environment degrades to a blank/transparent texture rather
// than garbage). Row 0 = top, matching this port's "no V-flip" convention established in
// initFromPath above (see its comment: GL's V=0 is the image's top row everywhere in this port) —
// a plain 2D canvas draws top-down already, so unlike the real engine's CGContext version (which
// has to flip because NSString draws in the UIKit referential) no flip is needed here.
#if defined(__EMSCRIPTEN__)
EM_JS(void, eden_rasterize_text_rgba, (const char* textC, int width, int height, float fontPx,
                                        int align, unsigned char* outPtr), {
  if (typeof document === 'undefined' && typeof OffscreenCanvas === 'undefined') return;
  var text = UTF8ToString(textC);
  var canvas = (typeof document !== 'undefined')
      ? document.createElement('canvas')
      : new OffscreenCanvas(width, height);
  canvas.width = width;
  canvas.height = height;
  var ctx = canvas.getContext('2d');
  if (!ctx) return;
  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = '#fff';
  ctx.font = fontPx + 'px sans-serif';
  ctx.textBaseline = 'middle';
  var x;
  if (align === 1) { ctx.textAlign = 'center'; x = width / 2; }
  else if (align === 2) { ctx.textAlign = 'right'; x = width; }
  else { ctx.textAlign = 'left'; x = 0; }
  ctx.fillText(text, x, height / 2);
  var img = ctx.getImageData(0, 0, width, height).data;
  HEAPU8.set(img, outPtr);
});
#else
// Phase N Stage 1: the ONE genuinely platform-shaped line in this otherwise-portable file. Native
// gets the same function from native/src/seam/TextRaster_native.cpp (stb_truetype), against this
// same signature and the same pre-zeroed-buffer contract — so everything below is shared.
extern "C" void eden_rasterize_text_rgba(const char* textC, int width, int height, float fontPx,
                                         int align, unsigned char* outPtr);
#endif

// Real call sites (NOT dead code — the header comment above this method used to claim the only
// caller was the commented-out Graphics::drawText; that's wrong, statusbar.mm's
// `new Texture2D(status, ...)` calls this directly, and that's what draws the world-name label
// under the menu's world picker and SharedList.mm's world/date labels). Ported for BEHAVIOR, not
// byte-for-byte: the real engine rasterizes via CGBitmapContext + NSString drawInRect; this port
// rasterizes via an HTML canvas 2D context (EM_JS above). Same POT-rounding/pixel-format/
// initData contract as the image path.
void Texture2D::initFromString(NSString* string, CGSize dimensions, UITextAlignment alignment, UIFont* font) {
    if (font == nil) {
        REPORT_ERROR(@"Invalid font", NULL);
        return;
    }
    int width = (int)dimensions.width;
    if (width != 1 && (width & (width - 1))) {
        int i = 1;
        while (i < width) i *= 2;
        width = i;
    }
    if (width > kMaxTextureSize_Eden) width = kMaxTextureSize_Eden;
    int height = (int)dimensions.height;
    if (height != 1 && (height & (height - 1))) {
        int i = 1;
        while (i < height) i *= 2;
        height = i;
    }
    if (height > kMaxTextureSize_Eden) height = kMaxTextureSize_Eden;
    if (width <= 0 || height <= 0) return;

    unsigned char* data = (unsigned char*)calloc((size_t)width * height * 4, 1);
    eden_rasterize_text_rgba([string UTF8String], width, height, [font pointSize], (int)alignment, data);

    initData(data, kTexture2DPixelFormat_RGBA8888, width, height, dimensions, FALSE);
    free(data);
}
Texture2D::Texture2D(NSString* string, CGSize dimensions, UITextAlignment alignment, UIFont* font) {
    initFromString(string, dimensions, alignment, font);
}

Texture2D::Texture2D(NSString* path) { initFromPath(path, NO, kTexture2DPixelFormat_Automatic, FALSE); }
Texture2D::Texture2D(NSString* path, BOOL sizeToFit, BOOL genMips) { initFromPath(path, sizeToFit, kTexture2DPixelFormat_Automatic, genMips); }
Texture2D::Texture2D(NSString* path, BOOL sizeToFit) { initFromPath(path, sizeToFit, kTexture2DPixelFormat_Automatic, FALSE); }
Texture2D::Texture2D(NSString* path, BOOL sizeToFit, Texture2DPixelFormat pixelFormat, BOOL genMips) { initFromPath(path, sizeToFit, pixelFormat, genMips); }

// Texture2D.mm's positional bookkeeping for the skin/mask art, same initial values as the engine
// (Texture2D.mm:566-568). Resources::loadResources zeroes each one immediately before the run of
// texture loads it is supposed to count, and initFromPath below advances them. -1 means "not
// counting" — that is what keeps every OTHER texture load out of the skin/mask slots.
// Defined here, above their first use, rather than at the foot of the file where they used to sit
// unused.
int storedMaskCounter = -1;
int storedSkinCounter = -1;
int realStoredSkinCounter = 0;

// The `stored*` UIImage globals are DEFINED in Classes/Resources.mm and filled in HERE, by the
// storeImage block below — exactly as Classes/Texture2D.mm does it. They are the input side of
// the recolor pipeline (Resources::getPaintTex/getPaintedTex/getDoorTex/getSkin read pixels back
// out of them), so leaving them unpopulated — which this file used to do — silently disabled
// every recolored icon in the game. Audit row 11 (A5).
extern UIImage* storedSkins[5][2];
extern UIImage* storedMasks[5][2];
extern UIImage* storedDoor;
extern UIImage* storedDoorMask;
extern UIImage* storedPaint;
extern UIImage* storedPaintMask;
extern UIImage* storedCube;
extern UIImage* storedCubeMask;
extern UIImage* storedFlowerico;
extern UIImage* storedFlowericoMask;
extern UIImage* storedDoorico;
extern UIImage* storedDooricoMask;
extern UIImage* storedPortalico;
extern UIImage* storedPortalicoMask;

void Texture2D::initFromPath(NSString* path, BOOL sizeToFit, Texture2DPixelFormat pixelFormat, BOOL genMips) {
    // ---- storeImage classification. Ported verbatim (control flow, not formatting) from
    // Classes/Texture2D.mm:608-683, and it has to stay verbatim: this is a POSITIONAL scheme, not
    // a lookup. Resources::loadResources zeroes storedSkinCounter right before it loads the 15
    // creature-skin PNGs and zeroes storedMaskCounter right before it loads the 10 MASK PNGs, so
    // "which slot does this image go in" is decided purely by how many textures have been loaded
    // since. Change the order of texture loads in Resources.mm and creatures get the wrong skins.
    //
    // Two consequences worth spelling out, because both are easy to "clean up" into a bug:
    //   * the counters advance BEFORE the decode and regardless of whether it succeeds, so a
    //     missing asset must not skip them (it would shift every later skin by one slot);
    //   * storedSkinCounter skips every third load (`%3 != 1`) because the skins come in
    //     Default/Rage/Blink triples and only two of the three are stored.
    BOOL isMask = FALSE;
    BOOL isDoor = FALSE;
    BOOL isPaint = FALSE;
    BOOL isGoldcubeico = FALSE;
    BOOL isFlowerico = FALSE;
    BOOL isDoorico = FALSE;
    BOOL isPortalico = FALSE;
    BOOL storeImage = FALSE;

    if (storedSkinCounter >= 0 && storedSkinCounter < 15) {
        if (storedSkinCounter % 3 != 1) {
            storeImage = TRUE;
        }
        storedSkinCounter++;
    }
    if (storedMaskCounter >= 0 && storedMaskCounter < 10) {
        isMask = TRUE;
        storeImage = TRUE;
    }
    if ([path isEqualToString:@"door.png"]) {
        isDoor = TRUE;
        storeImage = TRUE;
    } else if ([path isEqualToString:@"door_mask.png"]) {
        isDoor = TRUE;
        isMask = TRUE;
        storeImage = TRUE;
    } else if ([path isEqualToString:@"palette.png"]) {
        isPaint = TRUE;
        storeImage = TRUE;
    } else if ([path isEqualToString:@"paint_mask.png"]) {
        isPaint = TRUE;
        isMask = TRUE;
        storeImage = TRUE;
    } else if ([path isEqualToString:@"goldcube_icon.png"]) {
        isGoldcubeico = TRUE;
        storeImage = TRUE;
    } else if ([path isEqualToString:@"goldcube_icon_mask.png"]) {
        isGoldcubeico = TRUE;
        isMask = TRUE;
        storeImage = TRUE;
    } else if ([path isEqualToString:@"flower_icon.png"]) {
        isFlowerico = TRUE;
        storeImage = TRUE;
    } else if ([path isEqualToString:@"flower_icon_mask.png"]) {
        isFlowerico = TRUE;
        isMask = TRUE;
        storeImage = TRUE;
    } else if ([path isEqualToString:@"door_icon2.png"]) {
        isDoorico = TRUE;
        storeImage = TRUE;
    } else if ([path isEqualToString:@"door_icon2_mask.png"]) {
        isDoorico = TRUE;
        isMask = TRUE;
        storeImage = TRUE;
    } else if ([path isEqualToString:@"portal_icon2.png"]) {
        isPortalico = TRUE;
        storeImage = TRUE;
    } else if ([path isEqualToString:@"portal_icon2_mask.png"]) {
        isPortalico = TRUE;
        isMask = TRUE;
        storeImage = TRUE;
    }

    // ipad~ retina-variant probe — real engine behavior (Texture2D.mm), kept because
    // NSFileManager/NSBundle are both real (POSIX-backed) in this port, so the check is
    // meaningful: if a preloaded ipad~ variant exists, prefer it. Note this runs AFTER the
    // classification above, exactly as in the engine — the block matches on the bare name, and
    // paint in particular only ships as ipad~palette.png / ipad~paint_mask.png, so matching after
    // the swap would classify nothing at all.
    if (IS_IPAD || SUPPORTS_RETINA) {
        NSString* oipadPath = [NSString stringWithFormat:@"ipad~%@", path];
        NSString* ipadPath = [[NSBundle mainBundle] pathForResource:oipadPath ofType:nil];
        if ([[NSFileManager defaultManager] fileExistsAtPath:ipadPath]) {
            path = oipadPath;
        }
    }
    if (![path isAbsolutePath]) {
        path = [[NSBundle mainBundle] pathForResource:path ofType:nil];
    }

    int srcW = 0, srcH = 0;
    BOOL hasAlpha = NO;
    unsigned char* src = EdenLoadPNG(path, &srcW, &srcH, &hasAlpha);

    // stb_image hands back its own allocation; CGImage wants one it can free(). They are both
    // plain malloc'd blocks in this build (stb uses STBI_MALLOC == malloc unless overridden, and
    // this file does not override it), so ownership simply transfers — no copy.
    CGImageRef cg = src ? EdenCGImageCreateWithRGBA(src, srcW, srcH, hasAlpha) : 0;

    g_decodeLabel = [path UTF8String];
    initFromImage(cg, UIImageOrientationUp, sizeToFit, pixelFormat, genMips);
    g_decodeLabel = "<CGImage>";

    // Ownership, matching the engine's own polarity (Texture2D.mm's `if(storeImage){…} else
    // [uiImage release];`): the ~14 images the recolor pipeline reads back from are wrapped in a
    // UIImage and kept forever; the other ~110 are freed the moment their pixels are in GL.
    //
    // -initWithCGImage:, NOT +imageWithCGImage: — this runs before any frame, hence before any
    // real autorelease pool. See the note on those two methods in uikit_stubs.h; getting this
    // wrong keeps every decoded texture resident for the life of the tab.
    if (storeImage) {
        // Kept even when the decode failed (cg == 0): the engine stores whatever it got, and a
        // nil here only means the RECOLOR of that one asset no-ops — a strictly better failure
        // than shifting every subsequent skin into the wrong slot.
        UIImage* kept = cg ? [[UIImage alloc] initWithCGImage:cg] : nil;
        if (isPortalico) {
            if (isMask) storedPortalicoMask = kept; else storedPortalico = kept;
        } else if (isDoorico) {
            if (isMask) storedDooricoMask = kept; else storedDoorico = kept;
        } else if (isFlowerico) {
            if (isMask) storedFlowericoMask = kept; else storedFlowerico = kept;
        } else if (isGoldcubeico) {
            if (isMask) storedCubeMask = kept; else storedCube = kept;
        } else if (isPaint) {
            if (isMask) storedPaintMask = kept; else storedPaint = kept;
        } else if (isDoor) {
            if (isMask) storedDoorMask = kept; else storedDoor = kept;
        } else if (isMask) {
            storedMasks[storedMaskCounter / 2][storedMaskCounter % 2] = kept;
            storedMaskCounter++;
        } else {
            storedSkins[realStoredSkinCounter / 2][realStoredSkinCounter % 2] = kept;
            realStoredSkinCounter++;
        }
    } else {
        CGImageRelease(cg);
    }
}

// =============================================================================================
// Draw methods — mechanical, near-verbatim ports (client arrays + glBindTexture + glDrawArrays,
// all already handled by gl_es1_shim.cpp GROUP 2d). Byte-for-byte identical control flow to
// Classes/Texture2D.mm; only whitespace differs.
// =============================================================================================
void Texture2D::drawTexture(Texture2D* texture, CGPoint point, CGFloat depth) {
    GLfloat maxS = texture->_maxS, maxT = texture->_maxT,
            pixelsWide = texture->_width, pixelsHigh = texture->_height;
    GLfloat coordinates[] = { 0, maxT, maxS, maxT, 0, 0, maxS, 0 };
    GLfloat width = (GLfloat)pixelsWide * maxS, height = (GLfloat)pixelsHigh * maxT;
    GLfloat vertices[] = {
        -width / 2 + point.x, -height / 2 + point.y, depth,
         width / 2 + point.x, -height / 2 + point.y, depth,
        -width / 2 + point.x,  height / 2 + point.y, depth,
         width / 2 + point.x,  height / 2 + point.y, depth
    };
    glBindTexture(GL_TEXTURE_2D, texture->name);
    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glTexCoordPointer(2, GL_FLOAT, 0, coordinates);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Texture2D::preload() {
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    drawInRect(CGRectMake(-2, -2, 0.1, 0.1));
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
}

void Texture2D::drawAtPoint(CGPoint point) { drawAtPoint(point, 0.0, FALSE); }

void Texture2D::drawAtPoint(CGPoint point, CGFloat depth, BOOL center) {
    GLfloat coordinates[] = { 0, _maxT, _maxS, _maxT, 0, 0, _maxS, 0 };
    GLfloat width = (GLfloat)_width * _maxS, height = (GLfloat)_height * _maxT;
    // Both branches vertically center on point.y; only the horizontal anchor differs
    // (center: point.x is the midpoint; !center: point.x is the left edge) — matches the
    // original's separate cvertices/zvertices arrays exactly.
    GLfloat cvertices[] = {
        -width / 2 + point.x, -height / 2 + point.y, depth,
         width / 2 + point.x, -height / 2 + point.y, depth,
        -width / 2 + point.x,  height / 2 + point.y, depth,
         width / 2 + point.x,  height / 2 + point.y, depth
    };
    GLfloat zvertices[] = {
        point.x,         -height / 2 + point.y, depth,
        width + point.x, -height / 2 + point.y, depth,
        point.x,          height / 2 + point.y, depth,
        width + point.x,  height / 2 + point.y, depth
    };
    glBindTexture(GL_TEXTURE_2D, name);
    glVertexPointer(3, GL_FLOAT, 0, center ? cvertices : zvertices);
    glTexCoordPointer(2, GL_FLOAT, 0, coordinates);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Texture2D::drawInRect(CGRect rect) { drawInRect(rect, 0.0); }

void Texture2D::drawInRect(CGRect rect, CGFloat depth) {
    GLfloat coordinates[] = { 0, _maxT, _maxS, _maxT, 0, 0, _maxS, 0 };
    if (IS_IPAD) {
        rect.origin.x *= SCALE_WIDTH;
        rect.origin.y *= SCALE_HEIGHT;
        rect.size.width *= SCALE_WIDTH;
        rect.size.height *= SCALE_HEIGHT;
    }
    GLfloat vertices[] = {
        rect.origin.x, rect.origin.y, depth,
        rect.origin.x + rect.size.width, rect.origin.y, depth,
        rect.origin.x, rect.origin.y + rect.size.height, depth,
        rect.origin.x + rect.size.width, rect.origin.y + rect.size.height, depth
    };
    glBindTexture(GL_TEXTURE_2D, name);
    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glTexCoordPointer(2, GL_FLOAT, 0, coordinates);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Texture2D::drawInRect2(CGRect rect) {
    CGFloat depth = 0.0;
    GLfloat coordinates[] = { 0, _maxT, _maxS, _maxT, 0, 0, _maxS, 0 };
    if (IS_IPAD) {
        rect.origin.x *= SCALE_WIDTH;
        rect.origin.y *= SCALE_HEIGHT;
        rect.size.width *= 2;
        rect.size.height *= 2;
    }
    GLfloat vertices[] = {
        rect.origin.x, rect.origin.y, depth,
        rect.origin.x + rect.size.width, rect.origin.y, depth,
        rect.origin.x, rect.origin.y + rect.size.height, depth,
        rect.origin.x + rect.size.width, rect.origin.y + rect.size.height, depth
    };
    glBindTexture(GL_TEXTURE_2D, name);
    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glTexCoordPointer(2, GL_FLOAT, 0, coordinates);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Texture2D::drawText(CGRect rect) {
    drawText(rect, FALSE);
}

// flipX mirrors across the X axis (top<->bottom), matching Classes/Texture2D.mm's overload —
// used to turn the up-pointing jump icon into a down-pointing crouch icon.
void Texture2D::drawText(CGRect rect, BOOL flipX) {
    CGFloat depth = 0.0;
    GLfloat coordinates[] = { 0, flipX?0:_maxT, _maxS, flipX?0:_maxT, 0, flipX?_maxT:0, _maxS, flipX?_maxT:0 };
    GLfloat width = roundf((GLfloat)_width * _maxS), height = roundf((GLfloat)_height * _maxT);
    if (!IS_RETINA && SUPPORTS_RETINA) { width /= 2; height /= 2; }
    if (IS_IPAD) {
        rect.origin.x *= SCALE_WIDTH;
        rect.origin.y *= SCALE_HEIGHT;
        rect.origin.x = roundf(rect.origin.x);
        rect.origin.y = roundf(rect.origin.y);
    }
    GLfloat vertices[] = {
        rect.origin.x, rect.origin.y, depth,
        rect.origin.x + width, rect.origin.y, depth,
        rect.origin.x, rect.origin.y + height, depth,
        rect.origin.x + width, rect.origin.y + height, depth
    };
    glBindTexture(GL_TEXTURE_2D, name);
    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glTexCoordPointer(2, GL_FLOAT, 0, coordinates);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Texture2D::drawTextHalfsies(CGRect rect) {
    CGFloat depth = 0.0;
    GLfloat coordinates[] = { 0, _maxT, _maxS, _maxT, 0, 0, _maxS, 0 };
    GLfloat width = roundf((GLfloat)_width * _maxS), height = roundf((GLfloat)_height * _maxT);
    if (!IS_RETINA && SUPPORTS_RETINA) { width /= 2; height /= 2; }
    if (IS_IPAD) {
        rect.origin.x *= SCALE_WIDTH;
        rect.origin.y *= SCALE_HEIGHT;
        rect.origin.x = roundf(rect.origin.x);
        rect.origin.y = roundf(rect.origin.y);
    }
    GLfloat vertices[] = {
        rect.origin.x, rect.origin.y, depth,
        rect.origin.x + width, rect.origin.y, depth,
        rect.origin.x, rect.origin.y + height, depth,
        rect.origin.x + width, rect.origin.y + height, depth
    };
    glBindTexture(GL_TEXTURE_2D, name);
    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glTexCoordPointer(2, GL_FLOAT, 0, coordinates);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Texture2D::drawTextNoScale(CGRect rect) {
    CGFloat depth = 0.0;
    GLfloat coordinates[] = { 0, _maxT, _maxS, _maxT, 0, 0, _maxS, 0 };
    GLfloat width = roundf((GLfloat)_width * _maxS), height = roundf((GLfloat)_height * _maxT);
    if (!IS_RETINA && SUPPORTS_RETINA) { width /= 2; height /= 2; }
    GLfloat vertices[] = {
        rect.origin.x, rect.origin.y, depth,
        rect.origin.x + width, rect.origin.y, depth,
        rect.origin.x, rect.origin.y + height, depth,
        rect.origin.x + width, rect.origin.y + height, depth
    };
    glBindTexture(GL_TEXTURE_2D, name);
    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glTexCoordPointer(2, GL_FLOAT, 0, coordinates);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Texture2D::drawTextM(CGRect rect) {
    CGFloat depth = 0.0;
    GLfloat width = 480, height = roundf((GLfloat)_height * _maxT);
    if (IS_IPAD) {
        rect.origin.x *= SCALE_WIDTH;
        width = 1024;
        rect.origin.y *= SCALE_HEIGHT;
        rect.origin.x = roundf(rect.origin.x);
        rect.origin.y = roundf(rect.origin.y);
    }
    GLfloat vertices[] = {
        rect.origin.x, rect.origin.y, depth,
        rect.origin.x + width, rect.origin.y, depth,
        rect.origin.x, rect.origin.y + height, depth,
        rect.origin.x + width, rect.origin.y + height, depth
    };
    GLfloat coordinates[] = { 0, _maxT, _maxS, _maxT, 0, 0, _maxS, 0 };
    glBindTexture(GL_TEXTURE_2D, name);
    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glTexCoordPointer(2, GL_FLOAT, 0, coordinates);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Texture2D::drawNumbers(CGRect rect, int num) {
    if (num < 0 || num > 10) {
        printg("num out of bounds/n");
        if (num < 0) num = 0;
        if (num > 10) num = 10;
    }
    CGFloat depth = 0.0;
    float xoff = num * 1 / 16.0f;
    float xwidth = 1;
    if (num == 10) xwidth = 2;
    GLfloat coordinates[] = {
        xoff, _maxT,
        xoff + _maxS / 16.0f * xwidth, _maxT,
        xoff, 0,
        xoff + _maxS / 16.0f * xwidth, 0
    };
    GLfloat width = roundf((GLfloat)_width * _maxS / 16.0f), height = roundf((GLfloat)_height * _maxT);
    if (num == 10) width = roundf((GLfloat)_width * _maxS / 16.0f * 2);
    if (!IS_RETINA && SUPPORTS_RETINA) { width /= 2; height /= 2; }
    if (IS_IPAD) {
        rect.origin.x *= SCALE_WIDTH;
        rect.origin.y *= SCALE_HEIGHT;
        rect.origin.x = roundf(rect.origin.x);
        rect.origin.y = roundf(rect.origin.y);
    }
    GLfloat vertices[] = {
        rect.origin.x, rect.origin.y, depth,
        rect.origin.x + width, rect.origin.y, depth,
        rect.origin.x, rect.origin.y + height, depth,
        rect.origin.x + width, rect.origin.y + height, depth
    };
    glBindTexture(GL_TEXTURE_2D, name);
    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glTexCoordPointer(2, GL_FLOAT, 0, coordinates);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Texture2D::drawButton(Button button) {
    drawButton(button, FALSE);
}

void Texture2D::drawButton(Button button, BOOL flipX) {
    if (button.pressed) {
        button.size.width = roundf((GLfloat)_width * _maxS) / 2.0f;
        button.size.height = roundf((GLfloat)_height * _maxT) / 2.0f;
        float offx = button.size.width * .08f;
        float offy = button.size.height * .08f;
        CGRect rect = CGRectMake(button.origin.x + offx, button.origin.y + offy,
                                  button.size.width - offx * 2, button.size.height - offy * 2);
        CGFloat depth = 0.0;
        GLfloat coordinates[] = { 0, flipX?0:_maxT, _maxS, flipX?0:_maxT, 0, flipX?_maxT:0, _maxS, flipX?_maxT:0 };
        GLfloat width = rect.size.width, height = rect.size.height;
        if (IS_IPAD) {
            rect.origin.x *= SCALE_WIDTH;
            rect.origin.y *= SCALE_HEIGHT;
            width *= 2;
            height *= 2;
            rect.origin.x = roundf(rect.origin.x);
            rect.origin.y = roundf(rect.origin.y);
        }
        GLfloat vertices[] = {
            rect.origin.x, rect.origin.y, depth,
            rect.origin.x + width, rect.origin.y, depth,
            rect.origin.x, rect.origin.y + height, depth,
            rect.origin.x + width, rect.origin.y + height, depth
        };
        glBindTexture(GL_TEXTURE_2D, name);
        glVertexPointer(3, GL_FLOAT, 0, vertices);
        glTexCoordPointer(2, GL_FLOAT, 0, coordinates);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else {
        CGRect rect = CGRectMake(button.origin.x, button.origin.y, button.size.width, button.size.height);
        drawText(rect, flipX);
    }
}

void Texture2D::drawButton2(Button button) {
    if (button.pressed) {
        drawButton(button);
    } else {
        CGRect rect = CGRectMake(button.origin.x, button.origin.y, button.size.width, button.size.height);
        drawInRect(rect);
    }
}

void Texture2D::drawSky(CGRect rect, CGFloat depth) {
    float pitch = World::getWorld->cam->pitch;
    float sinPitch = sin(D2R(pitch));
    float sty = 0;
    float ety = 1.0f;
    if (sinPitch < 0) {
        sty = -sinPitch;
        if (sty > .8) sty = .8;
    } else {
        ety = 1 - sinPitch;
        if (ety < .2) ety = .2;
    }
    if (IS_IPAD) {
        rect.origin.x *= SCALE_WIDTH;
        rect.origin.y *= SCALE_HEIGHT;
        rect.size.width *= SCALE_WIDTH;
        rect.size.height *= SCALE_HEIGHT;
    }
    GLfloat coordinates[] = {
        0, ety * 32,
        _maxS * rect.size.width / 128, ety * 32,
        0, sty * 32,
        _maxS * rect.size.width / 128, sty * 32
    };
    GLfloat vertices[] = {
        rect.origin.x, rect.origin.y, depth,
        rect.origin.x + rect.size.width, rect.origin.y, depth,
        rect.origin.x, rect.origin.y + rect.size.height, depth,
        rect.origin.x + rect.size.width, rect.origin.y + rect.size.height, depth
    };
    glBindTexture(GL_TEXTURE_2D, name);
    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glTexCoordPointer(2, GL_FLOAT, 0, coordinates);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// =============================================================================================
// The recolor pipeline (audit row 11 / A5). Both of these were inert `return 0;` stubs until
// 2026-07-31, which is the whole of the invisible-paint-icon bug: Resources.mm feeds the result
// straight into `new Texture2D(cgimage, …)`, and a null there leaves `name == 0`, i.e. nothing
// drawn. Four live call sites depend on this — Hud.mm's paint icon (getPaintTex) and painted
// build icons (getPaintedTex), Terrain.mm's doors (getDoorTex), Model.mm's creature skins
// (getSkin).
//
// PORTED FOR BEHAVIOR, NOT FOR MECHANISM. The original (Classes/Texture2D.mm:210) does this by
// drawing both images into two ARGB CGBitmapContexts and walking them as `int*`; that `int` view
// is little-endian-dependent (kCGBitmapByteOrder32Big + AlphaPremultipliedFirst means the bytes
// are A,R,G,B, so the word reads back as A | R<<8 | G<<16 | B<<24 — which is exactly why the
// original's channel extraction looks shifted by one byte). This port already holds decoded
// pixels as straight RGBA8 bytes, so the byte-order dance has nothing to reproduce; what is
// reproduced exactly is the arithmetic.
//
// Two deliberate divergences from CoreGraphics, both recorded because they are the kind of thing
// a later reader will otherwise "fix":
//   1. STRAIGHT alpha, not premultiplied. CGContextDrawImage premultiplies on the way in; stb
//      never does, so the rest of this port is straight and mixing conventions would darken the
//      edges of exactly this subset of the art. Inside the recolored region the difference is
//      nil anyway — measured on the real assets, 487 of 490 masked pixels are alpha 255 (the
//      other three are 252-254) — and outside it the pixels are passed through untouched.
//   2. The mask is sampled nearest-neighbour if its dimensions differ from the image's, which is
//      what CGContextDrawImage's scale-into-rect would do. Every shipped pair is the same size
//      (checked: 90x90, 70x70, 32x64, 256x256), so this is robustness, not behavior.
// =============================================================================================

// Recolor `inImage` wherever `inMask` is opaque white, preserving the source's own luminance.
// `tint` is packed the way Resources.mm packs it: 0xBBGGRRAA, i.e. red at bits 8-15. Returns a
// NEW CGImage the caller owns (Resources.mm hands it to +[UIImage imageWithCGImage:], which
// adopts it — so unlike the original, which leaked one bitmap per recolor, nothing leaks here).
CGImageRef ManipulateImagePixelData(CGImageRef inImage, CGImageRef inMask, int tint) {
    if (!inImage || !inImage->rgba || !inMask || !inMask->rgba) {
        // The engine's own shape for this is `if (cgctx == NULL) { printg(...); return NULL; }`.
        printg("Texture2D_web: ManipulateImagePixelData with a null image/mask — recolor skipped\n");
        return 0;
    }
    const int w = inImage->width;
    const int h = inImage->height;
    const int mw = inMask->width;
    const int mh = inMask->height;

    unsigned char* out = (unsigned char*)malloc((size_t)w * h * 4);
    if (!out) return 0;
    memcpy(out, inImage->rgba, (size_t)w * h * 4);

    const float fr = ((tint >> 8) & 255) / 255.0f;
    const float fg = ((tint >> 16) & 255) / 255.0f;
    const float fb = ((tint >> 24) & 255) / 255.0f;

    for (int y = 0; y < h; ++y) {
        const int my = (mh == h) ? y : (int)((long long)y * mh / h);
        for (int x = 0; x < w; ++x) {
            const int mx = (mw == w) ? x : (int)((long long)x * mw / w);
            const unsigned char* m = inMask->rgba + ((size_t)my * mw + mx) * 4;
            // The original's test is `data2[i] == 0xFFFFFFFF` — opaque white, all four channels.
            // Anything softer (an anti-aliased mask edge) is NOT recolored, by design.
            if (m[0] != 255 || m[1] != 255 || m[2] != 255 || m[3] != 255) continue;

            unsigned char* d = out + ((size_t)y * w + x) * 4;
            // `grey` is the source pixel's max channel — HSV "value", not luminance. The original
            // spells this as `bb = MAX(MAX(bb,gg),rr)` and then divides bb by 255.
            int mx3 = d[0];
            if (d[1] > mx3) mx3 = d[1];
            if (d[2] > mx3) mx3 = d[2];
            const float grey = mx3 / 255.0f;
            d[0] = (unsigned char)(grey * fr * 255.0f);
            d[1] = (unsigned char)(grey * fg * 255.0f);
            d[2] = (unsigned char)(grey * fb * 255.0f);
            d[3] = 255;   // the original writes a literal 0xFF into the alpha byte here
        }
    }
    // Always alpha-bearing: the recolored region is forced opaque but the surrounding art keeps
    // its own alpha, and the icons are cut-outs. Matches the original, whose result came out of a
    // premultiplied-first context and therefore always reported an alpha channel to initFromImage.
    return EdenCGImageCreateWithRGBA(out, w, h, TRUE);
}

// The maskless variant. DEAD in this engine — every call site is commented out (Resources.mm:564,
// 591, 614) — but implemented rather than stubbed so the symbol does not lie about what it does,
// and because it is 20 lines. Faithfully reproduces the original's quirk of deriving `grey` from
// the BLUE channel alone (Texture2D.mm:513 `float grey=bb/255.0f`, where bb was never max'd the
// way the masked variant max's it) rather than "fixing" it to match ManipulateImagePixelData.
// `mode` is accepted and ignored, exactly as in the original.
CGImageRef ManipulateImagePixelData2(CGImageRef inImage, int tint, int mode) {
    (void)mode;
    if (!inImage || !inImage->rgba) return 0;
    const int w = inImage->width;
    const int h = inImage->height;
    unsigned char* out = (unsigned char*)malloc((size_t)w * h * 4);
    if (!out) return 0;
    memcpy(out, inImage->rgba, (size_t)w * h * 4);

    const float fr = ((tint >> 8) & 255) / 255.0f;
    const float fg = ((tint >> 16) & 255) / 255.0f;
    const float fb = ((tint >> 24) & 255) / 255.0f;

    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        unsigned char* d = out + i * 4;
        const float grey = d[2] / 255.0f;   // blue channel only — see the note above
        d[0] = (unsigned char)(grey * fr * 255.0f);
        d[1] = (unsigned char)(grey * fg * 255.0f);
        d[2] = (unsigned char)(grey * fb * 255.0f);
        d[3] = 255;
    }
    return EdenCGImageCreateWithRGBA(out, w, h, TRUE);
}
