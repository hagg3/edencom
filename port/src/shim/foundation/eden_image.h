// eden_image.h — G.2b-1 (WORKING/de-objc-plan-2026-10-24.md D11). The port's decoded-image handle, in a
// plain C/C++ header: one tightly-packed RGBA8 buffer, row 0 = TOP (the port's no-V-flip convention,
// Texture2D.mm initFromImage), STRAIGHT alpha (stb_image never premultiplies). It was `struct
// CGImage` in uikit_stubs.h (audit row 11 / A5), reached through UIImage wrappers; the engine now holds
// `EdenImage*` directly (Resources.mm's stored* recolour sources) and frees it with eden_image_free.
// (uikit_stubs.h and its `CGImageRef` typedef went in G.3; the dead stock CG paths that named it,
// Util.cpp's screenshot and FileManager::loadGenFromDisk, are preprocessed out.)
#ifndef EDEN_IMAGE_H
#define EDEN_IMAGE_H

#include <stdlib.h>
#include "eden_base.h"   // BOOL

struct EdenImage {
    int width;
    int height;
    bool hasAlpha;          // was the SOURCE alpha-bearing? drives kTexture2DPixelFormat_Automatic
    unsigned char *rgba;    // width*height*4, owned by this struct, freed by eden_image_free
};
typedef struct EdenImage EdenImage;

// Takes ownership of `rgbaOwned` (malloc'd, width*height*4). Null (and the buffer freed) on bad input.
static inline EdenImage *eden_image_create_rgba(unsigned char *rgbaOwned, int width, int height, bool hasAlpha) {
    if (!rgbaOwned || width <= 0 || height <= 0) { free(rgbaOwned); return 0; }
    EdenImage *img = (EdenImage *)malloc(sizeof(EdenImage));
    if (!img) { free(rgbaOwned); return 0; }
    img->width = width;
    img->height = height;
    img->hasAlpha = hasAlpha;
    img->rgba = rgbaOwned;
    return img;
}
static inline void eden_image_free(EdenImage *img) {
    if (!img) return;
    free(img->rgba);
    free(img);
}

#endif
