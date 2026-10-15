#import "uikit_stubs.h"

#include <stdlib.h>   // malloc/free — CGImage's own storage (audit row 11 / A5)
#include <stdint.h>   // intptr_t — see -isRealTouch (Phase N Stage 3)

@implementation EDEN_UIKIT_CLASS(UITouch)
- (CGPoint)locationInView:(UIView *)view { (void)view; return _location; }
- (UITouchPhase)phase { return _phase; }
- (double)timestamp { return _timestamp; }
// intptr_t, not long. `_identity` is a small signed integer smuggled through a void* (see
// uikit_stubs.h), and `long` is 32 bits on LLP64 Windows — so this was a hard "cast from pointer
// to smaller type" error there, and would have been a silent truncation had the compiler allowed
// it. intptr_t is the type that is pointer-width everywhere by definition. Phase N Stage 3.
- (BOOL)isRealTouch { return ((intptr_t)_identity) >= 0; }
// Explicit getter rather than @synthesize: the `view` property exists only so Input.mm's
// `touch.view` dot syntax parses (see uikit_stubs.h), and it feeds straight back into
// -locationInView:, which ignores it. Writing the getter by hand also avoids depending on
// clang's property auto-synthesis under -fobjc-runtime=gnustep-1.9.
- (UIView *)view { return nil; }
@end

@implementation EDEN_UIKIT_CLASS(UIEvent)
@end

@implementation EDEN_UIKIT_CLASS(UIImage)
// Real since audit row 11 (A5) — see uikit_stubs.h for the representation and why the ivars are
// POD. The two remaining stubs have no engine call site on any live path; they still return nil
// rather than aborting, for the reason the original comment gave: texture setup runs during world
// load, and a hard failure there stops the port from ever reaching a first frame. A missing
// texture is visible and diagnosable; an abort at load is not.
+ (UIImage *)imageNamed:(NSString *)name { (void)name; return nil; }
+ (UIImage *)imageWithContentsOfFile:(NSString *)path { (void)path; return nil; } // P4: dead-code link stub
- (id)initWithCGImage:(CGImageRef)cgImage {
    self = [super init];
    if (self) {
        _cgImage = cgImage;                  // ownership transfers; -dealloc releases it
        _orientation = UIImageOrientationUp;
    }
    return self;
}
+ (UIImage *)imageWithCGImage:(CGImageRef)cgImage {
    return [[[UIImage alloc] initWithCGImage:cgImage] autorelease];
}
- (CGImageRef)CGImage { return _cgImage; }
- (UIImageOrientation)imageOrientation { return _orientation; }
- (CGSize)size {
    CGSize z;
    z.width = _cgImage ? (float)_cgImage->width : 0.0f;
    z.height = _cgImage ? (float)_cgImage->height : 0.0f;
    return z;
}
- (void)drawInRect:(CGRect)rect { (void)rect; }   // TODO P2 — no engine call site
- (void)dealloc {
    CGImageRelease(_cgImage);
    _cgImage = 0;
    [super dealloc];
}
@end

@implementation EDEN_UIFONT_CLASS   // see uikit_stubs.h: renamed under EDEN_USE_SYSTEM_FOUNDATION
+ (EDEN_UIFONT_CLASS *)systemFontOfSize:(float)size {
    UIFont *f = [[[UIFont alloc] init] autorelease];
    f->_pointSize = size;
    return f;
}
- (float)pointSize { return _pointSize; }
@end

@implementation EDEN_UIKIT_CLASS(UIColor)
+ (UIColor *)colorWithRed:(float)r green:(float)g blue:(float)b alpha:(float)a {
    (void)r; (void)g; (void)b; (void)a;
    return [[[UIColor alloc] init] autorelease]; // TODO P2
}
@end

@implementation EDEN_UIKIT_CLASS(UIView)
- (void)insertSubview:(UIView *)view atIndex:(NSInteger)index { (void)view; (void)index; } // TODO P2/P3
@end

@implementation EDEN_UIKIT_CLASS(UIAccelerometer)
+ (UIAccelerometer *)sharedAccelerometer { return nil; } // TODO
@end

@implementation EDEN_UIKIT_CLASS(EAGLContext) {
@public
    EAGLRenderingAPI _api;
}
+ (EAGLContext *)currentContext {
    static EAGLContext *g_current = nil; // TODO P2: real WebGL2-context-backed tracking
    return g_current;
}
+ (BOOL)setCurrentContext:(EAGLContext *)context { (void)context; return YES; } // TODO P2
- (id)initWithAPI:(EAGLRenderingAPI)api {
    self = [super init];
    _api = api;
    return self;
}
@end

// --- CoreGraphics / UIGraphics, all TODO P2 -------------------------------------------------
// See uikit_stubs.h for why these exist and why they are likely to be deleted rather than
// implemented: the single caller is Util.mm's screenshot path, which on web becomes a readPixels
// plus canvas.toBlob(). Every one returns empty rather than aborting, for the same reason UIImage's
// methods do — a hard failure here would stop the world from loading, and the screenshot path is
// not on the critical path to a first frame.
extern "C" {

CGColorSpaceRef CGColorSpaceCreateDeviceRGB(void) { return 0; }
void CGColorSpaceRelease(CGColorSpaceRef cs) { (void)cs; }

CGDataProviderRef CGDataProviderCreateWithData(void *info, const void *data, size_t size,
                                               CGDataProviderReleaseDataCallback cb) {
  (void)info;
  (void)data;
  (void)size;
  (void)cb;
  // NOTE for P2: the real function takes ownership of `data` and calls `cb` to free it. Util.mm
  // relies on that — it allocates the pixel buffer, hands it over, and never frees it itself. A
  // real implementation must honor the callback contract or this leaks a full framebuffer per
  // screenshot.
  return 0;
}

CGImageRef CGImageCreate(size_t width, size_t height, size_t bitsPerComponent,
                         size_t bitsPerPixel, size_t bytesPerRow, CGColorSpaceRef space,
                         CGBitmapInfo bitmapInfo, CGDataProviderRef provider,
                         const float *decode, BOOL shouldInterpolate,
                         CGColorRenderingIntent intent) {
  (void)width; (void)height; (void)bitsPerComponent; (void)bitsPerPixel; (void)bytesPerRow;
  (void)space; (void)bitmapInfo; (void)provider; (void)decode; (void)shouldInterpolate;
  (void)intent;
  return 0;
}

// Real since audit row 11 (A5). Note this is the ONLY deallocator for a CGImage in the port, and
// it is reached two ways: UIImage's -dealloc (the normal path) and a direct call from a failure
// branch that never got as far as wrapping the image. CGImageCreate above still returns 0, so a
// null here is expected, not defensive noise.
void CGImageRelease(CGImageRef image) {
  if (!image) return;
  free(image->rgba);
  free(image);
}

// Real since audit row 11 (A5): ManipulateImagePixelData sizes its work from these.
size_t CGImageGetWidth(CGImageRef image) { return image ? (size_t)image->width : 0; }
size_t CGImageGetHeight(CGImageRef image) { return image ? (size_t)image->height : 0; }

CGImageRef EdenCGImageCreateWithRGBA(unsigned char *rgbaOwned, int width, int height,
                                     BOOL hasAlpha) {
  if (!rgbaOwned || width <= 0 || height <= 0) {
    free(rgbaOwned);
    return 0;
  }
  CGImageRef img = (CGImageRef)malloc(sizeof(struct CGImage));
  if (!img) {
    free(rgbaOwned);
    return 0;
  }
  img->width = width;
  img->height = height;
  img->hasAlpha = hasAlpha;
  img->rgba = rgbaOwned;
  return img;
}
CGContextRef CGBitmapContextCreate(void *data, size_t width, size_t height,
                                   size_t bitsPerComponent, size_t bytesPerRow,
                                   CGColorSpaceRef space, CGBitmapInfo bitmapInfo) {
  (void)data; (void)width; (void)height; (void)bitsPerComponent; (void)bytesPerRow;
  (void)space; (void)bitmapInfo;
  return 0;
}
void CGContextDrawImage(CGContextRef c, CGRect rect, CGImageRef image) { (void)c; (void)rect; (void)image; }
void CGContextRelease(CGContextRef c) { (void)c; }

void UIGraphicsBeginImageContext(CGSize size) { (void)size; }
UIImage *UIGraphicsGetImageFromCurrentImageContext(void) { return nil; }
void UIGraphicsEndImageContext(void) {}
NSData *UIImagePNGRepresentation(UIImage *image) { (void)image; return nil; }

}  // extern "C"

// --- App-shell singletons. See uikit_stubs.h for which calls are live and which are archaeology.
@implementation EDEN_UIKIT_CLASS(UIScreen)
+ (UIScreen *)mainScreen { return [[[UIScreen alloc] init] autorelease]; }   // TODO P2
- (CGRect)bounds { CGRect r; r.origin.x = 0; r.origin.y = 0; r.size.width = 0; r.size.height = 0; return r; } // TODO P2
- (float)scale { return 1.0f; }                                              // TODO P2
@end

@implementation EDEN_UIKIT_CLASS(UIDevice)
+ (UIDevice *)currentDevice { return [[[UIDevice alloc] init] autorelease]; }
- (NSInteger)orientation { return UIDeviceOrientationLandscapeLeft; }
@end

@implementation EDEN_UIKIT_CLASS(UIApplication)
+ (UIApplication *)sharedApplication { return [[[UIApplication alloc] init] autorelease]; }
// Genuinely nothing to do, not a deferred stub: a browser page has no status bar to hide.
- (void)setStatusBarHidden:(BOOL)hidden { (void)hidden; }
- (BOOL)openURL:(NSURL *)url { (void)url; return NO; }   // TODO P6 — window.open
@end
