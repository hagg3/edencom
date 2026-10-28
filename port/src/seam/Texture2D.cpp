// Texture2D.cpp — Stage P2 seam replacement for Classes/Texture2D.mm.
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
//      instead of UIImage/CGBitmapContext, replicating initFromImage's pow-of-two padding (only
//      for sizeToFit since Q.2: NPOT images upload at their own size), the upside-down-texture flip (Texture2D.h's own doc comment: "content will be upside-down"),
//      and the per-pixel-format packing (RGB565/RGBA4444/RGBA5551/RGB888) bit-for-bit.
//
// THE RECOLOR PIPELINE — implemented 2026-07-31 (project audit row 11 / A5), was a stub before.
// initFromImage(const EdenImage*, ...) (CGImageRef before G.2b-1) / ManipulateImagePixelData(2) / the
// storeImage bookkeeping are all live. This comment block used to say they were "DELIBERATELY NOT PORTED (P2b, later)" on
// the reasoning that the only call sites were creature skins, gated behind Model.mm's CPU-skinning
// rewrite. That reasoning was wrong on the facts: three of the four call sites have nothing to do
// with creatures — Hud.mm's paint icon and painted build icons, and Terrain.mm's doors — and all
// three were drawing NOTHING as a result. Worth keeping as a lesson: "this path is only reached by
// X" deserves a grep, not a recollection. Full write-up in web-port-final:web/docs/resources-and-audio.md; the
// standing regression guard is tools/headless-recolor-test.js.
//
// DELIBERATELY NOT PORTED:
//   - initFromString / the `(std::string, CGSize, UITextAlignment, float fontSize)` text-rasterizing
//     constructor. CORRECTION (see archive/PORT-STATUS-2026-08-13.md "Owed/open" + RESUME-HERE pass 35): the old
//     claim here — "confirmed dead code, only call site is the commented-out Graphics::drawText"
//     — was wrong. statusbar.mm's `setStatus` calls `new Texture2D(status, ...)` directly (the
//     world-name label under the menu's world picker, and SharedList.mm's world/date labels), and
//     that path is very much live. Implemented by port/src/seam/TextRaster_native.cpp (stb_truetype).
//     (This entry was itself a correction of an earlier wrong "dead code" claim — the same
//     mistake the recolor block above records. Two for two on that pattern in one file.)

// printg is normally supplied by the force-included eden_prefix.h (engine .mm sources only,
// per CMakeLists.txt's COMPILE_OPTIONS) — this seam file isn't in that list, so it needs its own
// definition before OpenGL_Internal.h's REPORT_ERROR/CHECK_GL_ERROR macros can use it. Matches
// the pch's __DEBUG__ branch (`printg(...) printf(__VA_ARGS__)`) unconditionally.
#define printg(...) printf(__VA_ARGS__)

#include "../../../Classes/Texture2D.h"
#include "../../../Classes/OpenGL_Internal.h"
#include "../../../Classes/Globals.h"
#include "../../../Classes/World.h"
#include "../../../Classes/Renderer.h"   // G.5b-3: uploads and draws are R::
#include "../../../Classes/Graphics.h"   // G.5b-7: the pen (Texture2D's quads draw with its state)
#include "../shim/foundation/eden_base.h"
#include "../shim/foundation/eden_image.h"
#include "../shim/foundation/eden_fs.h"      // G.2b-1: eden_bundle_path / eden_path_is_absolute / EdenFile
#include "../shim/foundation/eden_image.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO   // we read the file ourselves (EdenFile since G.2b-1; NSData before)
#include "stb_image.h"

#include "../shim/foundation/platform_shims.h"   // (Phase N Stage 1)

#include <cstdlib>
#include <cstring>
#include <algorithm>

// Texture2D.mm's kMaxTextureSize is a private #define local to that .mm, not in the header —
// redeclared here under a distinct name to avoid any accidental collision if that header ever
// changes.
static const int kMaxTextureSize_Eden = 1024;
// D.2t: the PNG path's cap. The block atlases grew to 32 x 2048 (64 tiles); under the 1024 cap they
// were halved to 16 x 1024 -- UVs still right, every tile half-res. 2048 is the ES 2.0-class floor every
// target clears (WebGL2 and ES 3.0 guarantee it). Strings keep kMaxTextureSize_Eden.
// H.14: the atlases are now 32 x 4096 (128 tiles), so the cap is 4096 -- at 2048 they were halved
// again. Every desktop GPU and every iOS device this port targets (A5 and later) does 4096.
static const int kMaxImageTextureSize_Eden = 4096;

// Diagnostic-only: the per-texture decode line below (and, since N.4.9, the shim's per-texture
// memory probe, via eden_gl_debug_tex_label) is far more useful with a filename on it, but
// initFromImage has no path (its other caller is the recolor pipeline, which has no file at
// all). initFromPath parks the name it is working on here for the duration of the call. Not
// thread-safe and does not need to be — CLAUDE.md convention #4, all of this runs on the one
// thread that owns GL. The two text constructors park "<text>" here the same way.
static const char* g_decodeLabel = "<image>";

extern "C" void eden_gl_debug_tex_label(const char* label);   // gl_textures.cpp (N.4.9)

// =============================================================================================
// initData — UNCHANGED FROM THE ENGINE. This constructor pair takes already-decoded raw pixels
// and is inline in the unmodified Texture2D.h... except initData() itself (the actual GL upload)
// is defined in the .mm, so it is ported here VERBATIM (byte-for-byte identical to
// Classes/Texture2D.mm's initData) — pure GL, no CoreGraphics, nothing to translate.
// =============================================================================================
void Texture2D::initData(const void* data, Texture2DPixelFormat pixelFormat, int width, int height,
                          CGSize size, bool genMips) {
    // G.5b-3: the upload is R::texCreate (port/src/render/gl/gl_renderer.cpp), which owns every upload rule.
    // The filters are the stock ones: with mips the atlas-style S repeat / T clamp and NEAREST_MIPMAP_LINEAR,
    // without (P.15) both repeat, no chain, level 0 only.
    R::TexFormat f;
    switch (pixelFormat) {
        case kTexture2DPixelFormat_RGBA8888: f = R::TF_RGBA8; break;
        case kTexture2DPixelFormat_RGBA4444: f = R::TF_RGBA4444; break;
        case kTexture2DPixelFormat_RGBA5551: f = R::TF_RGBA5551; break;
        case kTexture2DPixelFormat_RGB565:   f = R::TF_RGB565; break;
        case kTexture2DPixelFormat_RGB888:   f = R::TF_RGB8; break;
        case kTexture2DPixelFormat_L8:       f = R::TF_L8; break;
        case kTexture2DPixelFormat_A8:       f = R::TF_A8; break;
        case kTexture2DPixelFormat_LA88:     f = R::TF_LA88; break;
        default:
            abort();   // was [NSException raise:NSInternalInconsistencyException], uncaught: the same end.
                       // The four PVRTC formats stay in the enum but nothing creates them (G.1 deleted the loader).
    }
    R::TexDesc d = {};
    d.w = width; d.h = height; d.format = f;
    d.mipmaps = genMips; d.linear = false;
    d.clampS = false; d.clampT = genMips ? true : false;
    d.label = g_decodeLabel;   // N.4.9 probe only
    name = R::texCreate(d, data);
    if (!name) {
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
    initData(data, pixelFormat, width, height, size, false);
}
Texture2D::Texture2D(const void* data, Texture2DPixelFormat pixelFormat, int width, int height, CGSize size, bool genMips) {
    initData(data, pixelFormat, width, height, size, genMips);
}

Texture2D::~Texture2D() {
    if (name) R::texDestroy(name);
}

std::string Texture2D::description() {
    char buf[128];
    snprintf(buf, sizeof(buf), "< Texture2D| Name = %i | Dimensions = %ix%i | Coordinates = (%.2f, %.2f)>",
             name, (int)_width, (int)_height, (double)_maxS, (double)_maxT);
    return buf;
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
unsigned char* EdenLoadPNG(const std::string& path, int* outW, int* outH, bool* outHasAlpha,
                           bool* outGreyAlpha) {
    std::vector<uint8_t> fileData;
    if (EdenFile* f = path.empty() ? nullptr : EdenFile::openRead(path)) {   // G.2b-1: was NSData
        const long long n = f->seekEnd();
        if (n > 0 && n < 0x7fffffff && f->seek(0)) fileData = f->read((size_t)n);
        delete f;
    }
    if (fileData.empty()) {
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
        printg("Texture2D: no data at '%s' (load failed or empty)\n", path.empty() ? "(null)" : path.c_str());
        return nullptr;
    }
    int comp = 0;
    unsigned char* pixels = stbi_load_from_memory(
        fileData.data(), (int)fileData.size(), outW, outH, &comp, 4);
    if (!pixels) {
        printg("Texture2D: stb_image failed to decode '%s': %s\n",
               path.c_str(), stbi_failure_reason());
        return nullptr;
    }
    if (outHasAlpha) *outHasAlpha = (comp == 4 || comp == 2) ? true : false;
    if (outGreyAlpha) *outGreyAlpha = (comp == 2) ? true : false;
    return pixels;
}

// Same power-of-two rounding rule as the engine's own (Texture2D.mm initFromImage): a dimension
// that is ALREADY a power of two (or exactly 1) is left untouched — only a non-power-of-two
// dimension gets rounded, and sizeToFit changes the rounding target from "next pow2 >= v" to
// "next pow2 such that 2x it >= v" (i.e. one power looser, so scaling up to fill loses less).
int EdenRoundDimension(int v, bool sizeToFit) {
    if (v == 1 || (v & (v - 1)) == 0) return v; // already a power of two (or 1): unchanged
    int i = 1;
    while ((sizeToFit ? 2 * i : i) < v) i *= 2;
    return i;
}

// Q.2 (F1): the texels a row must be a multiple of for GL's default GL_UNPACK_ALIGNMENT of 4,
// which nothing in the port changes. A POT row of 4+ texels always was; an NPOT one in a 16-bit,
// RGB888 or 8-bit format may not be, and would upload sheared.
int EdenRowAlignTexels(Texture2DPixelFormat f) {
    switch (f) {
        case kTexture2DPixelFormat_RGBA4444:
        case kTexture2DPixelFormat_RGBA5551:
        case kTexture2DPixelFormat_RGB565:
        case kTexture2DPixelFormat_LA88:   return 2;
        case kTexture2DPixelFormat_RGB888:
        case kTexture2DPixelFormat_L8:
        case kTexture2DPixelFormat_A8:     return 4;
        default:                           return 1;
    }
}

}  // namespace

// Real since audit row 11 (A5) — this used to be an empty stub, and everything downstream of it
// (paint icon, painted build icons, doors, creature skins) drew nothing as a result.
//
// This now carries the whole POT-rounding / canvas-placement / pixel-format-packing pipeline that
// used to live inline in initFromPath, because that is the engine's own shape: Texture2D.mm's
// initFromPath decodes to a UIImage and then calls initFromImage([uiImage CGImage], …). Both of
// this port's sources — a PNG off the preloaded FS and a freshly recolored buffer out of
// ManipulateImagePixelData — arrive here as an EdenImage and take exactly one code path.
//
// There is no `orientation` parameter since G.2b-1 (D11): it was accepted and ignored, because every
// image the port produces is UIImageOrientationUp and the real engine's initFromImage only consulted it
// to build a CGAffineTransform for a CGContext this port does not have.
void Texture2D::initFromImage(const EdenImage* image, bool sizeToFit,
                               Texture2DPixelFormat pixelFormat, bool genMips) {
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
    // Q.2 (F1): no POT canvas unless the caller asked for scaling. ES 3.0 and desktop core GL take
    // NPOT textures with REPEAT and mips, and the POT padding was 14.4 MiB of 45.2 decoded (the
    // pinwheel alone 16 -> 9.5 MiB). sizeToFit keeps the stock rule: "resample onto a POT grid" is
    // what it asks for, and its only callers (atlas.png, atlas2.png) are POT already. Every reader
    // of the canvas size is a draw method below, through _maxS/_maxT (plan: WORKING/
    // npot-textures-plan-2026-10-22.md); engine code that binds `name` with its own UVs only does
    // so for textures that are POT, whose canvas is unchanged.
    int width = sizeToFit ? EdenRoundDimension(srcW, sizeToFit) : srcW;
    int height = sizeToFit ? EdenRoundDimension(srcH, sizeToFit) : srcH;
    // Oversized cap: proportional halving (not a hard clamp to kMaxTextureSize_Eden), matching
    // Texture2D.mm's `while((width>kMax)||(height>kMax)){width/=2;height/=2;...}` — rare in
    // practice (the tallest asset is the 2048-row block atlas, D.2t) but kept for fidelity.
    while (width > kMaxImageTextureSize_Eden || height > kMaxImageTextureSize_Eden) {
        width /= 2;
        height /= 2;
    }
    // Q.2: an NPOT row gets a transparent column or three so it stays 4-byte aligned; _maxS
    // already excludes them. A POT width is left as the stock rule made it.
    if (width & (width - 1)) {
        const int a = EdenRowAlignTexels(pixelFormat);
        width = (width + a - 1) / a * a;
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
        printg("Texture2D: '%s' decoded %dx%d (canvas %dx%d) fmt=%d center-rgba=(%d,%d,%d,%d)\n",
               g_decodeLabel, srcW, srcH, width, height, (int)pixelFormat, p[0], p[1], p[2], p[3]);
    }

    initData(finalData, pixelFormat, width, height, imageSize, genMips);

    if (finalData != toFree) free(finalData);
    free(toFree);
}
Texture2D::Texture2D(const EdenImage* image, bool sizeToFit,
                     Texture2DPixelFormat pixelFormat, bool genMips) {
    initFromImage(image, sizeToFit, pixelFormat, genMips);
}

// Rasterizes `textC` into an RGBA8 buffer at `outPtr` (width*height*4 bytes, caller-owned,
// pre-zeroed so a headless/no-canvas environment degrades to a blank/transparent texture rather
// than garbage). Row 0 = top, matching this port's "no V-flip" convention established in
// initFromPath above (see its comment: GL's V=0 is the image's top row everywhere in this port) —
// a plain 2D canvas draws top-down already, so unlike the real engine's CGContext version (which
// has to flip because NSString draws in the UIKit referential) no flip is needed here.
// Phase N Stage 1: the ONE genuinely platform-shaped line in this otherwise-portable file. Native
// gets the same function from port/src/seam/TextRaster_native.cpp (stb_truetype), against this
// same signature and the same pre-zeroed-buffer contract — so everything below is shared.
extern "C" void eden_rasterize_text_rgba(const char* textC, int width, int height, float fontPx,
                                         int align, unsigned char* outPtr);

// Real call sites (NOT dead code — the header comment above this method used to claim the only
// caller was the commented-out Graphics::drawText; that's wrong, statusbar.mm's
// `new Texture2D(status, ...)` calls this directly, and that's what draws the world-name label
// under the menu's world picker and SharedList.mm's world/date labels). Ported for BEHAVIOR, not
// byte-for-byte: the real engine rasterizes via CGBitmapContext + NSString drawInRect; this port
// rasterizes via a platform text rasterizer (TextRaster_native.cpp). Same POT-rounding/pixel-format/
// initData contract as the image path.
void Texture2D::initFromString(const std::string& string, CGSize dimensions, UITextAlignment alignment, float fontSize) {
    if (!(fontSize > 0.0f)) {   // was a nil UIFont* (no caller ever passed one)
        printg("Texture2D: invalid font size %g\n", (double)fontSize);
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
    eden_rasterize_text_rgba(string.c_str(), width, height, fontSize, (int)alignment, data);

    g_decodeLabel = "<text>";
    initData(data, kTexture2DPixelFormat_RGBA8888, width, height, dimensions, false);
    g_decodeLabel = "<image>";
    free(data);
}
// Delegates so a box wider than the texture cap takes the reduced-density path below (Stage 5.6).
Texture2D::Texture2D(const std::string& string, CGSize dimensions, UITextAlignment alignment, float fontSize)
    : Texture2D(string, dimensions, alignment, fontSize, 1.0f) {}

// N.4.8 — UI text at the device's real density. The stock path rasterises one texel per 2D-ortho
// unit, and the ortho (SCREEN_* x SCALE_*) is not the drawable: on an iPad Air 2's touch profile
// it is 1136x852 units across a 2046x1536 px box, so every label was magnified 1.8x through
// GL_MAG_FILTER NEAREST — the doubled-pixel softness reported as "dialogs slightly blurry".
//
// So rasterise at W = round(w * density) texels (font scaled to match) and upload the W x H
// raster as it is (Q.2; it used to be copied into the top-left of a POT buffer). Rasterising into
// a POT buffer directly would not do: the seam aligns text against the WHOLE buffer width, and
// pot(w * density) is not w * density, so centred and right-aligned text would move. drawText
// then draws W / density units — within half a device pixel of `w`, and exactly 1:1 on pixels.
//
// THE CAP, AND THE DOUBLED STATUS LINE (Stage 5.6). A box wider than kMaxTextureSize_Eden used to
// fall through to initFromString(), which clamps the TEXTURE to the cap but keeps the box as its
// content size — so _maxS came out > 1 (2276 / 1024 for the menu's full-width status bar on a
// 1138-point screen) and GL_REPEAT drew the line two and a bit times side by side. That is the
// stock menu's "Choose world to load" printed twice, and the HUD's status line had it too. Such a
// box now takes this path at a density BELOW 1: the raster fits the cap, and drawing divides by
// _density, so the label still spans the box it was laid out for — just at fewer texels.
Texture2D::Texture2D(const std::string& string, CGSize dimensions, UITextAlignment alignment, float fontSize,
                     float density) {
    if (!(fontSize > 0.0f)) {
        initFromString(string, dimensions, alignment, fontSize);   // reports the error
        return;
    }
    // X.20: below 1 is honoured too (a 1x monitor's small window): a 1:1 raster at fewer texels
    // beats a 1-texel-per-unit one minified through NEAREST.
    if (!(density > 0.0f)) density = 1.0f;
    // A label that would outgrow the texture cap gets less density, never a clipped raster.
    if (dimensions.width  * density > kMaxTextureSize_Eden) density = kMaxTextureSize_Eden / (float)dimensions.width;
    if (dimensions.height * density > kMaxTextureSize_Eden) density = kMaxTextureSize_Eden / (float)dimensions.height;
    if (density == 1.0f) {
        initFromString(string, dimensions, alignment, fontSize);
        return;
    }
    int W = (int)lroundf((float)dimensions.width * density);
    int H = (int)lroundf((float)dimensions.height * density);
    if (W > kMaxTextureSize_Eden) W = kMaxTextureSize_Eden;
    if (H > kMaxTextureSize_Eden) H = kMaxTextureSize_Eden;
    if (W <= 0 || H <= 0) return;

    // Q.2 (F1): uploaded at W x H. This used to copy the raster into the top-left of a POT buffer
    // and let _maxS/_maxT select it; RGBA8888 rows are always 4-byte aligned, so nothing else owed.
    unsigned char* data = (unsigned char*)calloc((size_t)W * H * 4, 1);
    eden_rasterize_text_rgba(string.c_str(), W, H, fontSize * density, (int)alignment, data);
    g_decodeLabel = "<text>";
    initData(data, kTexture2DPixelFormat_RGBA8888, W, H, CGSizeMake(W, H), false);
    g_decodeLabel = "<image>";
    free(data);
    _density = density;
}

Texture2D::Texture2D(const std::string& path) { initFromPath(path, false, kTexture2DPixelFormat_Automatic, false); }
Texture2D::Texture2D(const std::string& path, bool sizeToFit, bool genMips) { initFromPath(path, sizeToFit, kTexture2DPixelFormat_Automatic, genMips); }
Texture2D::Texture2D(const std::string& path, bool sizeToFit) { initFromPath(path, sizeToFit, kTexture2DPixelFormat_Automatic, false); }
Texture2D::Texture2D(const std::string& path, bool sizeToFit, Texture2DPixelFormat pixelFormat, bool genMips) { initFromPath(path, sizeToFit, pixelFormat, genMips); }

// Texture2D.mm's positional bookkeeping for the skin/mask art, same initial values as the engine
// (Texture2D.mm:566-568). Resources::loadResources zeroes each one immediately before the run of
// texture loads it is supposed to count, and initFromPath below advances them. -1 means "not
// counting" — that is what keeps every OTHER texture load out of the skin/mask slots.
// Defined here, above their first use, rather than at the foot of the file where they used to sit
// unused.
int storedMaskCounter = -1;
int storedSkinCounter = -1;
int realStoredSkinCounter = 0;

// The `stored*` EdenImage globals (UIImage* before G.2b-1) are DEFINED in Classes/Resources.mm and filled in HERE, by the
// storeImage block below — exactly as Classes/Texture2D.mm does it. They are the input side of
// the recolor pipeline (Resources::getPaintTex/getPaintedTex/getDoorTex/getSkin read pixels back
// out of them), so leaving them unpopulated — which this file used to do — silently disabled
// every recolored icon in the game. Audit row 11 (A5).
extern EdenImage* storedSkins[5][2];
extern EdenImage* storedMasks[5][2];
extern EdenImage* storedDoor;
extern EdenImage* storedDoorMask;
extern EdenImage* storedPaint;
extern EdenImage* storedPaintMask;
extern EdenImage* storedCube;
extern EdenImage* storedCubeMask;
extern EdenImage* storedFlowerico;
extern EdenImage* storedFlowericoMask;
extern EdenImage* storedDoorico;
extern EdenImage* storedDooricoMask;
extern EdenImage* storedPortalico;
extern EdenImage* storedPortalicoMask;

void Texture2D::initFromPath(const std::string& name, bool sizeToFit, Texture2DPixelFormat pixelFormat, bool genMips) {
    std::string path = name;
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
    bool isMask = false;
    bool isDoor = false;
    bool isPaint = false;
    bool isGoldcubeico = false;
    bool isFlowerico = false;
    bool isDoorico = false;
    bool isPortalico = false;
    bool storeImage = false;

    if (storedSkinCounter >= 0 && storedSkinCounter < 15) {
        if (storedSkinCounter % 3 != 1) {
            storeImage = true;
        }
        storedSkinCounter++;
    }
    if (storedMaskCounter >= 0 && storedMaskCounter < 10) {
        isMask = true;
        storeImage = true;
    }
    if (path == "door.png") {
        isDoor = true;
        storeImage = true;
    } else if (path == "door_mask.png") {
        isDoor = true;
        isMask = true;
        storeImage = true;
    } else if (path == "palette.png") {
        isPaint = true;
        storeImage = true;
    } else if (path == "paint_mask.png") {
        isPaint = true;
        isMask = true;
        storeImage = true;
    } else if (path == "goldcube_icon.png") {
        isGoldcubeico = true;
        storeImage = true;
    } else if (path == "goldcube_icon_mask.png") {
        isGoldcubeico = true;
        isMask = true;
        storeImage = true;
    } else if (path == "flower_icon.png") {
        isFlowerico = true;
        storeImage = true;
    } else if (path == "flower_icon_mask.png") {
        isFlowerico = true;
        isMask = true;
        storeImage = true;
    } else if (path == "door_icon2.png") {
        isDoorico = true;
        storeImage = true;
    } else if (path == "door_icon2_mask.png") {
        isDoorico = true;
        isMask = true;
        storeImage = true;
    } else if (path == "portal_icon2.png") {
        isPortalico = true;
        storeImage = true;
    } else if (path == "portal_icon2_mask.png") {
        isPortalico = true;
        isMask = true;
        storeImage = true;
    }

    // ipad~ retina-variant probe — real engine behavior (Texture2D.mm): if a shipped ipad~ variant
    // exists, prefer it (eden_bundle_path answers "" on a miss, like Apple's pathForResource). Note this runs AFTER the
    // classification above, exactly as in the engine — the block matches on the bare name, and
    // paint in particular only ships as ipad~palette.png / ipad~paint_mask.png, so matching after
    // the swap would classify nothing at all.
    if (IS_IPAD || SUPPORTS_RETINA) {
        const std::string oipadPath = "ipad~" + path;
        const std::string ipadPath = eden_bundle_path(oipadPath);
        if (!ipadPath.empty() && eden_fs_exists(ipadPath)) {
            path = oipadPath;
        }
    }
    if (!eden_path_is_absolute(path)) {
        path = eden_bundle_path(path);
    }

    int srcW = 0, srcH = 0;
    bool hasAlpha = false, greyAlpha = false;
    unsigned char* src = EdenLoadPNG(path, &srcW, &srcH, &hasAlpha, &greyAlpha);
    // Q.4 (F3): a grey+alpha PNG (the menu pinwheel, 9.5 MiB as RGBA) uploads as LA88 at half the
    // bytes; it samples identically (L -> RGB). Only "Automatic", whose RGBA8888 choice came from
    // the alpha alone; the stored EdenImage below stays RGBA8, so recolors are unaffected. Desktop
    // core GL has no LUMINANCE_ALPHA: the shim's glTexImage2D maps it to RG8 + a swizzle.
    if (greyAlpha && pixelFormat == kTexture2DPixelFormat_Automatic)
        pixelFormat = kTexture2DPixelFormat_LA88;

    // stb_image hands back its own allocation; EdenImage wants one it can free(). They are both
    // plain malloc'd blocks in this build (stb uses STBI_MALLOC == malloc unless overridden, and
    // this file does not override it), so ownership simply transfers — no copy.
    EdenImage* cg = src ? eden_image_create_rgba(src, srcW, srcH, hasAlpha) : 0;

    g_decodeLabel = path.c_str();
    initFromImage(cg, sizeToFit, pixelFormat, genMips);
    g_decodeLabel = "<image>";

    // Ownership, matching the engine's own polarity (Texture2D.mm's `if(storeImage){…} else
    // [uiImage release];`): the ~14 images the recolor pipeline reads back from are kept (as the
    // EdenImage itself since G.2b-1; a UIImage wrapping it before); the other ~110 are freed the
    // moment their pixels are in GL.
    if (storeImage) {
        // Kept even when the decode failed (cg == 0): the engine stores whatever it got, and a
        // nil here only means the RECOLOR of that one asset no-ops — a strictly better failure
        // than shifting every subsequent skin into the wrong slot.
        EdenImage* kept = cg;
        EdenImage** slot;
        if (isPortalico) {
            slot = isMask ? &storedPortalicoMask : &storedPortalico;
        } else if (isDoorico) {
            slot = isMask ? &storedDooricoMask : &storedDoorico;
        } else if (isFlowerico) {
            slot = isMask ? &storedFlowericoMask : &storedFlowerico;
        } else if (isGoldcubeico) {
            slot = isMask ? &storedCubeMask : &storedCube;
        } else if (isPaint) {
            slot = isMask ? &storedPaintMask : &storedPaint;
        } else if (isDoor) {
            slot = isMask ? &storedDoorMask : &storedDoor;
        } else if (isMask) {
            slot = &storedMasks[storedMaskCounter / 2][storedMaskCounter % 2];
            storedMaskCounter++;
        } else {
            slot = &storedSkins[realStoredSkinCounter / 2][realStoredSkinCounter % 2];
            realStoredSkinCounter++;
        }
        // Q.3: the game textures now reload on every world entry (DEFER_GAME_TEXTURES), so a slot
        // can already hold the previous load's image. Overwriting it leaked ~5 MB a world load
        // (--leak-probe). The readers in Resources.mm only borrow the image for one recolor.
        eden_image_free(*slot);
        *slot = kept;
    } else {
        eden_image_free(cg);
    }
}

// =============================================================================================
// Draw methods — mechanical, near-verbatim ports (client arrays + glBindTexture + glDrawArrays,
// all already handled by gl_es1_shim.cpp GROUP 2d). Byte-for-byte identical control flow to
// Classes/Texture2D.mm; only whitespace differs.
// =============================================================================================
namespace {
// G.5b-3: every Texture2D quad is one R:: strip of 4 interleaved (xyz, uv) vertices. Blend / depth /
// cull and the colour are what the caller (Hud, the menus, the kits) set on the pen (G.5b-7; until then
// the shim's logical state, read through R::legacy*()).
// The UI program has no fog, texture matrix or alpha test: the draw trace showed every Texture2D
// call site with fog off, texmat I and alpha off.
struct QuadVertex { float x, y, z, u, v; };
void fillQuad(QuadVertex q[4], const float* xyz, const float* st) {
    for (int i = 0; i < 4; ++i) { q[i].x = xyz[i * 3]; q[i].y = xyz[i * 3 + 1]; q[i].z = xyz[i * 3 + 2]; q[i].u = st[i * 2]; q[i].v = st[i * 2 + 1]; }
}
R::Geometry quadGeometry(const QuadVertex* q) {
    R::Geometry g = {};
    g.fmt = &R::kFmtUI3UV; g.vmem = q; g.itype = R::I_NONE; g.first = 0; g.count = 4; g.prim = R::TRIANGLE_STRIP;
    return g;
}
void drawQuad(uint32_t tex, const float* xyz, const float* st) {
    QuadVertex q[4];
    fillQuad(q, xyz, st);
    R::setState(Pen::state());
    R::draw2D(quadGeometry(q), tex, Pen::rgba());
}
// drawSky: inside Terrain::render's atlas texture matrix (R::uv() scale (1, 1/ATLAS_TILES)), so the world
// program (which owns that transform), no alpha test. Since G.5b-6 Terrain::render (the only caller)
// states the sky's R::State and fog (off) itself and passes the tint.
void drawSkyQuad(uint32_t tex, const float* xyz, const float* st, const R::Rgba& tint) {
    QuadVertex q[4];
    fillQuad(q, xyz, st);
    R::drawWorld(quadGeometry(q), tex, tint, -1.0f);
}
}  // namespace

void Texture2D::drawTexture(Texture2D* texture, CGPoint point, CGFloat depth) {
    float maxS = texture->_maxS, maxT = texture->_maxT,
            pixelsWide = texture->_width, pixelsHigh = texture->_height;
    float coordinates[] = { 0, maxT, maxS, maxT, 0, 0, maxS, 0 };
    float width = (float)pixelsWide * maxS, height = (float)pixelsHigh * maxT;
    float vertices[] = {
        -width / 2 + point.x, -height / 2 + point.y, depth,
         width / 2 + point.x, -height / 2 + point.y, depth,
        -width / 2 + point.x,  height / 2 + point.y, depth,
         width / 2 + point.x,  height / 2 + point.y, depth
    };
    drawQuad(texture->name, vertices, coordinates);
}

void Texture2D::preload() {
    R::proj().push();
    R::proj().loadIdentity();
    R::mv().push();
    R::mv().loadIdentity();
    drawInRect(CGRectMake(-2, -2, 0.1, 0.1));
    R::mv().pop();
    R::proj().pop();
}

void Texture2D::drawAtPoint(CGPoint point) { drawAtPoint(point, 0.0, false); }

void Texture2D::drawAtPoint(CGPoint point, CGFloat depth, bool center) {
    float coordinates[] = { 0, _maxT, _maxS, _maxT, 0, 0, _maxS, 0 };
    float width = (float)_width * _maxS / _density, height = (float)_height * _maxT / _density;  // N.4.8
    // Both branches vertically center on point.y; only the horizontal anchor differs
    // (center: point.x is the midpoint; !center: point.x is the left edge) — matches the
    // original's separate cvertices/zvertices arrays exactly.
    float cvertices[] = {
        -width / 2 + point.x, -height / 2 + point.y, depth,
         width / 2 + point.x, -height / 2 + point.y, depth,
        -width / 2 + point.x,  height / 2 + point.y, depth,
         width / 2 + point.x,  height / 2 + point.y, depth
    };
    float zvertices[] = {
        point.x,         -height / 2 + point.y, depth,
        width + point.x, -height / 2 + point.y, depth,
        point.x,          height / 2 + point.y, depth,
        width + point.x,  height / 2 + point.y, depth
    };
    drawQuad(name, center ? cvertices : zvertices, coordinates);
}

void Texture2D::drawInRect(CGRect rect) { drawInRect(rect, 0.0); }

void Texture2D::drawInRect(CGRect rect, CGFloat depth) {
    float coordinates[] = { 0, _maxT, _maxS, _maxT, 0, 0, _maxS, 0 };
    if (IS_IPAD) {
        rect.origin.x *= SCALE_WIDTH;
        rect.origin.y *= SCALE_HEIGHT;
        rect.size.width *= SCALE_WIDTH;
        rect.size.height *= SCALE_HEIGHT;
    }
    float vertices[] = {
        rect.origin.x, rect.origin.y, depth,
        rect.origin.x + rect.size.width, rect.origin.y, depth,
        rect.origin.x, rect.origin.y + rect.size.height, depth,
        rect.origin.x + rect.size.width, rect.origin.y + rect.size.height, depth
    };
    drawQuad(name, vertices, coordinates);
}

void Texture2D::drawInRect2(CGRect rect) {
    CGFloat depth = 0.0;
    float coordinates[] = { 0, _maxT, _maxS, _maxT, 0, 0, _maxS, 0 };
    if (IS_IPAD) {
        rect.origin.x *= SCALE_WIDTH;
        rect.origin.y *= SCALE_HEIGHT;
        rect.size.width *= 2;
        rect.size.height *= 2;
    }
    float vertices[] = {
        rect.origin.x, rect.origin.y, depth,
        rect.origin.x + rect.size.width, rect.origin.y, depth,
        rect.origin.x, rect.origin.y + rect.size.height, depth,
        rect.origin.x + rect.size.width, rect.origin.y + rect.size.height, depth
    };
    drawQuad(name, vertices, coordinates);
}

void Texture2D::drawText(CGRect rect) {
    drawText(rect, false);
}

// flipX mirrors across the X axis (top<->bottom), matching Classes/Texture2D.mm's overload —
// used to turn the up-pointing jump icon into a down-pointing crouch icon.
void Texture2D::drawText(CGRect rect, bool flipX) {
    CGFloat depth = 0.0;
    float coordinates[] = { 0, flipX?0:_maxT, _maxS, flipX?0:_maxT, 0, flipX?_maxT:0, _maxS, flipX?_maxT:0 };
    float width = roundf((float)_width * _maxS), height = roundf((float)_height * _maxT);
    if (!IS_RETINA && SUPPORTS_RETINA) { width /= 2; height /= 2; }
    // N.4.8: a density-built label is `_density` texels per unit, and its origin is snapped to a
    // DEVICE pixel rather than an ortho unit so those texels stay 1:1. Both are no-ops at 1.
    width /= _density; height /= _density;
    if (IS_IPAD) {
        rect.origin.x *= SCALE_WIDTH;
        rect.origin.y *= SCALE_HEIGHT;
        rect.origin.x = roundf(rect.origin.x * _density) / _density;
        rect.origin.y = roundf(rect.origin.y * _density) / _density;
    }
    float vertices[] = {
        rect.origin.x, rect.origin.y, depth,
        rect.origin.x + width, rect.origin.y, depth,
        rect.origin.x, rect.origin.y + height, depth,
        rect.origin.x + width, rect.origin.y + height, depth
    };
    drawQuad(name, vertices, coordinates);
}

void Texture2D::drawTextHalfsies(CGRect rect) {
    CGFloat depth = 0.0;
    float coordinates[] = { 0, _maxT, _maxS, _maxT, 0, 0, _maxS, 0 };
    float width = roundf((float)_width * _maxS), height = roundf((float)_height * _maxT);
    if (!IS_RETINA && SUPPORTS_RETINA) { width /= 2; height /= 2; }
    if (IS_IPAD) {
        rect.origin.x *= SCALE_WIDTH;
        rect.origin.y *= SCALE_HEIGHT;
        rect.origin.x = roundf(rect.origin.x);
        rect.origin.y = roundf(rect.origin.y);
    }
    float vertices[] = {
        rect.origin.x, rect.origin.y, depth,
        rect.origin.x + width, rect.origin.y, depth,
        rect.origin.x, rect.origin.y + height, depth,
        rect.origin.x + width, rect.origin.y + height, depth
    };
    drawQuad(name, vertices, coordinates);
}

void Texture2D::drawTextNoScale(CGRect rect) {
    CGFloat depth = 0.0;
    float coordinates[] = { 0, _maxT, _maxS, _maxT, 0, 0, _maxS, 0 };
    float width = roundf((float)_width * _maxS), height = roundf((float)_height * _maxT);
    if (!IS_RETINA && SUPPORTS_RETINA) { width /= 2; height /= 2; }
    float vertices[] = {
        rect.origin.x, rect.origin.y, depth,
        rect.origin.x + width, rect.origin.y, depth,
        rect.origin.x, rect.origin.y + height, depth,
        rect.origin.x + width, rect.origin.y + height, depth
    };
    drawQuad(name, vertices, coordinates);
}

void Texture2D::drawTextM(CGRect rect) {
    CGFloat depth = 0.0;
    float width = 480, height = roundf((float)_height * _maxT);
    if (IS_IPAD) {
        rect.origin.x *= SCALE_WIDTH;
        width = 1024;
        rect.origin.y *= SCALE_HEIGHT;
        rect.origin.x = roundf(rect.origin.x);
        rect.origin.y = roundf(rect.origin.y);
    }
    float vertices[] = {
        rect.origin.x, rect.origin.y, depth,
        rect.origin.x + width, rect.origin.y, depth,
        rect.origin.x, rect.origin.y + height, depth,
        rect.origin.x + width, rect.origin.y + height, depth
    };
    float coordinates[] = { 0, _maxT, _maxS, _maxT, 0, 0, _maxS, 0 };
    drawQuad(name, vertices, coordinates);
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
    float coordinates[] = {
        xoff, _maxT,
        xoff + _maxS / 16.0f * xwidth, _maxT,
        xoff, 0,
        xoff + _maxS / 16.0f * xwidth, 0
    };
    float width = roundf((float)_width * _maxS / 16.0f), height = roundf((float)_height * _maxT);
    if (num == 10) width = roundf((float)_width * _maxS / 16.0f * 2);
    if (!IS_RETINA && SUPPORTS_RETINA) { width /= 2; height /= 2; }
    if (IS_IPAD) {
        rect.origin.x *= SCALE_WIDTH;
        rect.origin.y *= SCALE_HEIGHT;
        rect.origin.x = roundf(rect.origin.x);
        rect.origin.y = roundf(rect.origin.y);
    }
    float vertices[] = {
        rect.origin.x, rect.origin.y, depth,
        rect.origin.x + width, rect.origin.y, depth,
        rect.origin.x, rect.origin.y + height, depth,
        rect.origin.x + width, rect.origin.y + height, depth
    };
    drawQuad(name, vertices, coordinates);
}

void Texture2D::drawButton(Button button) {
    drawButton(button, false);
}

void Texture2D::drawButton(Button button, bool flipX) {
    if (button.pressed) {
        button.size.width = roundf((float)_width * _maxS) / 2.0f;
        button.size.height = roundf((float)_height * _maxT) / 2.0f;
        float offx = button.size.width * .08f;
        float offy = button.size.height * .08f;
        CGRect rect = CGRectMake(button.origin.x + offx, button.origin.y + offy,
                                  button.size.width - offx * 2, button.size.height - offy * 2);
        CGFloat depth = 0.0;
        float coordinates[] = { 0, flipX?0:_maxT, _maxS, flipX?0:_maxT, 0, flipX?_maxT:0, _maxS, flipX?_maxT:0 };
        float width = rect.size.width, height = rect.size.height;
        if (IS_IPAD) {
            rect.origin.x *= SCALE_WIDTH;
            rect.origin.y *= SCALE_HEIGHT;
            width *= 2;
            height *= 2;
            rect.origin.x = roundf(rect.origin.x);
            rect.origin.y = roundf(rect.origin.y);
        }
        float vertices[] = {
            rect.origin.x, rect.origin.y, depth,
            rect.origin.x + width, rect.origin.y, depth,
            rect.origin.x, rect.origin.y + height, depth,
            rect.origin.x + width, rect.origin.y + height, depth
        };
        drawQuad(name, vertices, coordinates);
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

void Texture2D::drawSky(CGRect rect, CGFloat depth, float r, float g, float b, float a) {
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
    // X.8: stock wrote `* 32` here because the sky draws inside Terrain::render's atlas texture
    // matrix, glScalef(1, 1/32, 1), so 0..32 spanned the whole sky_box.png. D.2t made that scale
    // 1/ATLAS_TILES (128 since H.14) and left this at 32: only the top quarter of the gradient (the
    // deep blue) reached the screen, the pale horizon band the fog colour blends into never did.
    float coordinates[] = {
        0, ety * ATLAS_TILES,
        _maxS * rect.size.width / 128, ety * ATLAS_TILES,
        0, sty * ATLAS_TILES,
        _maxS * rect.size.width / 128, sty * ATLAS_TILES
    };
    float vertices[] = {
        rect.origin.x, rect.origin.y, depth,
        rect.origin.x + rect.size.width, rect.origin.y, depth,
        rect.origin.x, rect.origin.y + rect.size.height, depth,
        rect.origin.x + rect.size.width, rect.origin.y + rect.size.height, depth
    };
    const R::Rgba tint = { r, g, b, a };
    drawSkyQuad(name, vertices, coordinates, tint);
}

// =============================================================================================
// The recolor pipeline (audit row 11 / A5). Both of these were inert `return 0;` stubs until
// 2026-07-31, which is the whole of the invisible-paint-icon bug: Resources.mm feeds the result
// straight into `new Texture2D(image, …)`, and a null there leaves `name == 0`, i.e. nothing
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
// NEW EdenImage the caller owns (Resources.mm frees it once the Texture2D has copied it; it handed it
// to +[UIImage imageWithCGImage:] before G.2b-1 — unlike the original, which leaked one bitmap per
// recolor, nothing leaks here).
EdenImage* ManipulateImagePixelData(const EdenImage* inImage, const EdenImage* inMask, int tint) {
    if (!inImage || !inImage->rgba || !inMask || !inMask->rgba) {
        // The engine's own shape for this is `if (cgctx == NULL) { printg(...); return NULL; }`.
        printg("Texture2D: ManipulateImagePixelData with a null image/mask — recolor skipped\n");
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
    return eden_image_create_rgba(out, w, h, true);
}

// The maskless variant. DEAD in this engine — every call site is commented out (Resources.mm:564,
// 591, 614) — but implemented rather than stubbed so the symbol does not lie about what it does,
// and because it is 20 lines. Faithfully reproduces the original's quirk of deriving `grey` from
// the BLUE channel alone (Texture2D.mm:513 `float grey=bb/255.0f`, where bb was never max'd the
// way the masked variant max's it) rather than "fixing" it to match ManipulateImagePixelData.
// `mode` is accepted and ignored, exactly as in the original.
EdenImage* ManipulateImagePixelData2(const EdenImage* inImage, int tint, int mode) {
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
    return eden_image_create_rgba(out, w, h, true);
}
