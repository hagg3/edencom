// uikit_stubs.h — companion to the D3a Foundation shim, NOT one of the original "~8 Foundation
// classes" the plan named, but required anyway: grep shows several ENGINE (non-seam) files
// reference UIKit/CoreGraphics types directly and cannot be modified —
//   Classes/Input.h (ENGINE, never touched — its real signature is
//     `void touchesBegan(NSSet* touches, UIEvent* event)` etc.)
//   Classes/Util.mm, Classes/Resources.mm, Classes/statusbar.mm (CGPoint/CGRect/CGSize,
//     UIImage*/UIFont* fields and calls)
// See foundation-usage.md "UIKit-adjacent types" for the full grep-verified breakdown of what
// needs real behavior (CGPoint/CGRect/CGSize, UITouch, UIEvent) vs. what's opaque/P2-deferred
// (UIImage, UIFont, UIColor, UIView, UIAccelerometer — Texture2D/statusbar raster surface).
#ifndef EDEN_SHIM_UIKIT_STUBS_H
#define EDEN_SHIM_UIKIT_STUBS_H

#import "NSObject.h"

#include <stddef.h>   // size_t — used by the CoreGraphics image declarations below

// ---- PHASE N STAGE 4.1: ON iOS, EVERY CLASS BELOW ALREADY EXISTS FOR REAL ---------------------
//
// This is the UIFont problem from Stage 1 (see the long note at EDEN_UIFONT_CLASS further down),
// generalised to the whole file, because iOS is the platform where the collision is not one name
// but all of them: SDL3's UIKit backend links UIKit and OpenGLES, so `UIView`, `UITouch`,
// `UIImage`, `UIScreen`, `UIApplication`, `EAGLContext` and the rest are registered by Apple's
// own frameworks in this process. Two classes with one name is not a warning to live with —
// libobjc keeps one arbitrarily, and if it keeps Apple's then `[[UITouch alloc] init]` in
// web/src/seam/Input_web.mm builds a real UITouch with none of this port's ivars and the first
// `_location` read is a wild pointer.
//
// The fix is the same tool, applied uniformly: the CLASS is registered under a `Eden_`-prefixed
// name, and `@compatibility_alias` keeps the SOURCE spelling — which engine files this port does
// not edit (Classes/Input.h, Classes/Util.mm, Classes/Resources.mm, Classes/statusbar.mm) use —
// resolving to it. Nothing outside these two files changes, on any target.
//
// WHY THE `@class` FORWARD DECLARATIONS COME FIRST: @compatibility_alias needs its target class
// to be declared already, and the aliases have to be in effect before the first @interface below
// (which is written in terms of the aliased names — `- (CGPoint)locationInView:(UIView*)view`).
// Declaring all ten up front and aliasing them in one block is the only order that satisfies
// both. It is also why nothing in this file may `#include <UIKit/...>`: the port shadows that
// framework (web/src/shim/foundation/framework/UIKit/), and an alias whose name is also a real
// @interface in the same translation unit is an error rather than a nicety.
//
// NOT DONE HERE: @protocol UITextFieldDelegate further down, which real UIKit also declares.
// Protocols have no @compatibility_alias, duplicate protocol definitions are merged by the
// runtime rather than fought over, and no engine code performs a conformance check on it — it
// exists so Classes/EAGLView.h's `<UITextFieldDelegate>` parses.
#if defined(EDEN_PLATFORM_IOS)
#define EDEN_UIKIT_CLASS(name) Eden_##name
@class Eden_UITouch;
@class Eden_UIEvent;
@class Eden_UIScreen;
@class Eden_UIDevice;
@class Eden_UIApplication;
@class Eden_UIImage;
@class Eden_UIColor;
@class Eden_UIView;
@class Eden_UIAccelerometer;
@class Eden_EAGLContext;
@compatibility_alias UITouch         Eden_UITouch;
@compatibility_alias UIEvent         Eden_UIEvent;
@compatibility_alias UIScreen        Eden_UIScreen;
@compatibility_alias UIDevice        Eden_UIDevice;
@compatibility_alias UIApplication   Eden_UIApplication;
@compatibility_alias UIImage         Eden_UIImage;
@compatibility_alias UIColor         Eden_UIColor;
@compatibility_alias UIView          Eden_UIView;
@compatibility_alias UIAccelerometer Eden_UIAccelerometer;
@compatibility_alias EAGLContext     Eden_EAGLContext;
#else
#define EDEN_UIKIT_CLASS(name) name
#endif

@class NSData;
@class EDEN_UIKIT_CLASS(UIImage);   // declared below; the CoreGraphics block references it first
@class NSURL;     // NSURL.h is included AFTER this header (via NSString.h), so forward-declare

// ---- CoreGraphics geometry — plain structs, real behavior (trivial, zero risk) ----
//
// Phase N Stage 1: a native Apple target has these for real, and real Foundation drags
// <CoreGraphics/CGGeometry.h> in via NSGeometry.h whether we want it or not — so defining them
// again here is a redefinition error, not a choice. The real ones differ only in that CGFloat is
// double rather than float, which no engine call site depends on (they are passed and stored,
// never bit-compared).
//
// NOTE WHAT IS *NOT* CONDITIONAL: the CGImage layer further down stays THIS PORT'S OWN on every
// target. That is deliberate and load-bearing — web/src/seam/Texture2D_web.mm and
// Classes/Resources.mm's paint/door/skin recolour pipeline are written against its representation
// (one tightly-packed RGBA8 buffer, row 0 = top, STRAIGHT alpha), and CoreGraphics has no
// non-premultiplied RGBA8 bitmap context to reproduce it with. Keeping it ours is what lets the
// native target compile Texture2D_web.mm unchanged. It works because nothing here includes
// <CoreGraphics/CGImage.h>: real Foundation pulls in CGBase/CGGeometry only.
#if defined(EDEN_USE_SYSTEM_FOUNDATION)
#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGGeometry.h>
#else
typedef struct { float x, y; } CGPoint;
typedef struct { float width, height; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;

// Util.mm initialises its thumbnail rect from CGRectZero before filling in the size.
static const CGRect CGRectZero = {{0, 0}, {0, 0}};

static inline CGPoint CGPointMake(float x, float y) { return (CGPoint){x, y}; }
static inline CGSize CGSizeMake(float w, float h) { return (CGSize){w, h}; }
static inline CGRect CGRectMake(float x, float y, float w, float h) {
    return (CGRect){{x, y}, {w, h}};
}
#endif

// ---- UITouch / UIEvent — real minimal behavior, load-bearing for Input.h (see file header).
// Populated by the web input seam (Stage P3, src/seam/EAGLView_web.mm's touch/pointer-event
// remap) rather than by any real UIKit — this is OUR object, constructed on the web side and
// handed to Input::touchesBegan/Moved/Ended/Cancelled exactly like the original EAGLView.mm
// handed real UITouch/UIEvent instances from CoreOSTouch.
typedef enum {
    UITouchPhaseBegan, UITouchPhaseMoved, UITouchPhaseStationary,
    UITouchPhaseEnded, UITouchPhaseCancelled
} UITouchPhase;

@class EDEN_UIKIT_CLASS(UIView);

@interface EDEN_UIKIT_CLASS(UITouch) : NSObject {
@public
    CGPoint _location;      // in the same pixel space EAGLView.mm's -locationInView: returned
    UITouchPhase _phase;
    double _timestamp;
    void *_identity;        // opaque per-pointer id (e.g. Pointer Event's pointerId), used so
                             // the web seam can find "the same touch" across move/end events —
                             // Input.mm itself never reads this field, only compares UITouch*
                             // pointer identity (grep-confirmed: it stores `UITouch* touch_id`
                             // per Classes/Input.h and compares pointer equality).
}
- (CGPoint)locationInView:(UIView *)view;
- (UITouchPhase)phase;
- (double)timestamp;
// Answers "did this come from an actual touchscreen/pointer gesture, as opposed to a synthetic
// touch the web seam manufactures to drive a HUD button from the keyboard (Input_web.mm's
// kJumpTouchIdentity/kHudTapIdentity/kClickIdentity, all negative) or the mouse (MOUSE_IDENTITY,
// also negative)?" Real `Touch.identifier` values from the browser are always >= 0 (Input_web.mm's
// own comment on this). Exists so engine code (Classes/Hud.mm, Joystick.mm) can ask a semantic
// question without knowing about identity numbering — keeps that scheme, a platform detail, in
// this shim rather than in Classes/.
- (BOOL)isRealTouch;
// Classes/Input.mm calls `[touch locationInView:touch.view]` (3 sites), so `view` has to be
// readable as a PROPERTY, not just as a method — dot syntax on a plain method is an error.
// It always resolves to the one GL view, and -locationInView: ignores its argument anyway
// (the web seam stores touch coordinates already in that view's pixel space), so this returns
// nil rather than inventing a UIView instance for the engine to pass straight back.
@property(nonatomic, readonly) UIView *view;
@end

// grep shows the engine only ever passes UIEvent* through (Input.h's signature requires one)
// without reading any field off it — a marker type is sufficient.
@interface EDEN_UIKIT_CLASS(UIEvent) : NSObject
@end

// ---- CGImageRef / UIImageOrientation / UITextAlignment.
//
// CGImage is REAL as of the audit-row-11 (A5) recolor work — no longer the opaque P2 placeholder
// this comment used to describe. It is the port's decoded-pixel handle, and the ONLY thing that
// makes `Resources::getPaintTex`/`getPaintedTex`/`getDoorTex`/`getSkin` work: those four read
// pixels back out of one image, tint them through a mask, and hand the result to Texture2D's
// `Texture2D(CGImageRef, ...)` constructor. With CGImage opaque they were handed a null and the
// paint/door/creature-tint art drew as nothing (`name` stayed 0).
//
// The representation is deliberately NOT CoreGraphics-shaped: one tightly-packed RGBA8 buffer,
// row 0 = TOP (matching this port's no-V-flip convention, see Texture2D_web.mm initFromImage),
// STRAIGHT (non-premultiplied) alpha — because stb_image never premultiplies, so every other
// texture in this port is straight too, and mixing the two conventions is how you get dark
// fringes on exactly one subset of the UI. The real CoreGraphics path premultiplies inside
// CGBitmapContext; where that mattered (the recolored region) the source art is alpha==255, so
// the two agree there — measured on ipad~palette.png/ipad~paint_mask.png: 487/490 masked pixels
// are alpha 255 and the other three are 252-254.
struct CGImage {
    int width;
    int height;
    BOOL hasAlpha;          // was the SOURCE 4-channel? drives kTexture2DPixelFormat_Automatic
    unsigned char *rgba;    // width*height*4, owned by this struct, freed by CGImageRelease
};
typedef struct CGImage *CGImageRef;
typedef enum {
    UIImageOrientationUp, UIImageOrientationDown, UIImageOrientationLeft,
    UIImageOrientationRight, UIImageOrientationUpMirrored, UIImageOrientationDownMirrored,
    UIImageOrientationLeftMirrored, UIImageOrientationRightMirrored
} UIImageOrientation;
typedef enum {
    UITextAlignmentLeft, UITextAlignmentCenter, UITextAlignmentRight
} UITextAlignment;

// ---- UIApplication / UIScreen / UIDevice — the app-shell singletons.
//
// Only three of these calls survive in NON-seam engine code (the rest are in files this port
// replaces or in commented-out archaeology, which CLAUDE.md convention #6 says to leave alone):
//   * World.mm's `[[UIApplication sharedApplication] setStatusBarHidden:YES]` — no-op on web,
//     there is no status bar; fullscreen is the Fullscreen API, and that is Stage P7's business.
//   * `openURL:` — maps to `window.open`, TODO P6 (it opens the sharing site).
//   * AppController.mm's `[UIScreen mainScreen]` bounds/scale — this is where the engine learns
//     its screen metrics. TODO P2: those come from canvas size × devicePixelRatio instead
//     (web-port-plan.md Stage P2, CLAUDE.md convention #3 on IS_IPAD meaning "2× UI scale").
//
// UIDevice appears ONLY inside commented-out orientation code in World.mm; it is declared here
// so that archaeology keeps parsing if anyone un-comments it, and for no other reason.
@interface EDEN_UIKIT_CLASS(UIScreen) : NSObject
+ (UIScreen *)mainScreen;      // TODO P2 — canvas size × devicePixelRatio
- (CGRect)bounds;              // TODO P2
- (float)scale;                // TODO P2
@end

@interface EDEN_UIKIT_CLASS(UIDevice) : NSObject
+ (UIDevice *)currentDevice;
- (NSInteger)orientation;
@end

typedef enum {
    UIDeviceOrientationUnknown, UIDeviceOrientationPortrait,
    UIDeviceOrientationPortraitUpsideDown, UIDeviceOrientationLandscapeLeft,
    UIDeviceOrientationLandscapeRight
} UIDeviceOrientation;

@interface EDEN_UIKIT_CLASS(UIApplication) : NSObject
+ (UIApplication *)sharedApplication;
- (void)setStatusBarHidden:(BOOL)hidden;   // no-op on web — no status bar exists
- (BOOL)openURL:(NSURL *)url;              // TODO P6 — window.open
@end

// ---- CoreGraphics image construction + the UIGraphics image-context stack — all TODO P2.
// Used only by Classes/Util.mm's screenshot path (takeScreenshot: reads the GL framebuffer,
// wraps the pixels in a CGImage, optionally rescales through a UIGraphics image context, and
// PNG-encodes the result for the world-preview upload). Reached via its
// `#include <QuartzCore/QuartzCore.h>`, which on iOS transitively provides CoreGraphics — see
// framework/QuartzCore/QuartzCore.h.
//
// On web the whole path collapses to `canvas.toBlob()` plus a readPixels, so these are very
// likely to be DELETED at Stage P2 rather than implemented one-for-one. They are declared with
// faithful signatures anyway, because Util.mm passes their results to each other and a wrong
// return type surfaces as a confusing type error three calls away (that is exactly how UIImage's
// -CGImage was found).
typedef struct CGColorSpace *CGColorSpaceRef;
typedef struct CGDataProvider *CGDataProviderRef;
typedef struct CGContext *CGContextRef;   // P4: link-only, for FileManager::loadGenFromDisk (dead code)
typedef unsigned int CGBitmapInfo;

enum { kCGBitmapByteOrderDefault = 0 };
enum { kCGRenderingIntentDefault = 0 };
// P4: only the two constants FileManager::loadGenFromDisk() ORs together are needed. That path
// is DEAD in this build (JUST_TERRAIN_GEN==0), so the values are link-fodder, not behaviour.
enum { kCGImageAlphaPremultipliedLast = 1 };
enum { kCGBitmapByteOrder32Big = (4 << 12) };
typedef int CGColorRenderingIntent;

typedef void (*CGDataProviderReleaseDataCallback)(void *info, const void *data, size_t size);

#ifdef __cplusplus
extern "C" {
#endif

CGColorSpaceRef CGColorSpaceCreateDeviceRGB(void);                                    // TODO P2
void CGColorSpaceRelease(CGColorSpaceRef cs);                                         // TODO P2
CGDataProviderRef CGDataProviderCreateWithData(void *info, const void *data, size_t size,
                                               CGDataProviderReleaseDataCallback cb);  // TODO P2
CGImageRef CGImageCreate(size_t width, size_t height, size_t bitsPerComponent,
                         size_t bitsPerPixel, size_t bytesPerRow, CGColorSpaceRef space,
                         CGBitmapInfo bitmapInfo, CGDataProviderRef provider,
                         const float *decode, BOOL shouldInterpolate,
                         CGColorRenderingIntent intent);                               // TODO P2
void CGImageRelease(CGImageRef image);                                                 // TODO P2

// Real since audit row 11 (A5): ManipulateImagePixelData sizes its work from these, exactly as
// the engine's own CreateARGBBitmapContext(inImage) does.
size_t CGImageGetWidth(CGImageRef image);
size_t CGImageGetHeight(CGImageRef image);
// Creates a CGImage that TAKES OWNERSHIP of `rgbaOwned` (width*height*4, straight alpha, row 0 =
// top). The buffer must come from malloc/calloc — CGImageRelease free()s it.
CGImageRef EdenCGImageCreateWithRGBA(unsigned char *rgbaOwned, int width, int height,
                                     BOOL hasAlpha);
// P4: link-only for the DEAD (JUST_TERRAIN_GEN) FileManager::loadGenFromDisk path. Not on any
// runtime path in this build — never actually invoked, so the bodies are inert.
CGContextRef CGBitmapContextCreate(void *data, size_t width, size_t height,
                                   size_t bitsPerComponent, size_t bytesPerRow,
                                   CGColorSpaceRef space, CGBitmapInfo bitmapInfo);
void CGContextDrawImage(CGContextRef c, CGRect rect, CGImageRef image);
void CGContextRelease(CGContextRef c);

void UIGraphicsBeginImageContext(CGSize size);                                         // TODO P2
UIImage *UIGraphicsGetImageFromCurrentImageContext(void);                              // TODO P2
void UIGraphicsEndImageContext(void);                                                  // TODO P2
NSData *UIImagePNGRepresentation(UIImage *image);                                      // TODO P2

#ifdef __cplusplus
}
#endif

// ---- UIImage — REAL as of audit row 11 (A5). It is the engine's image handle everywhere it
// loads a texture or builds one procedurally: Resources.mm keeps a dozen `UIImage*` globals
// (`storedPaint`, `storedPaintMask`, `storedSkins[5][2]`, …) that Texture2D::initFromPath fills
// in as the art loads, then recolors pixels out of one into another. That whole pipeline used to
// be dead here — `imageNamed:`/`imageWithCGImage:` returned nil and `-CGImage` returned 0 — which
// is precisely why the paint/door icons and creature tints drew as nothing.
//
// A UIImage is now a thin retain-counted owner of one CGImage. Both ivars are POD, deliberately:
// `class_createInstance()` is a bare `calloc` and this port's hand-written ObjC runtime emits no
// `.cxx_construct`/`.cxx_destruct`, so a non-POD C++ ivar in an @implementation is never
// constructed and never destroyed (pass 56 / audit row A12 — the write-up is at the top of
// NSUserDefaults.mm). A zeroed pointer pair is a valid empty image; an `std::vector` would not be.
//
// The signatures have to stay exact, because Resources.mm feeds the results straight into
// Texture2D's C++ constructor: `new Texture2D([uiImage2 CGImage], [uiImage2 imageOrientation], …)`.
// Declaring -CGImage as returning `id` (the default for an undeclared selector) makes that
// constructor call fail to match, which is how these methods were found.
@interface EDEN_UIKIT_CLASS(UIImage) : NSObject {
@public
    CGImageRef _cgImage;                 // owned; released in -dealloc
    UIImageOrientation _orientation;
}
+ (UIImage *)imageNamed:(NSString *)name;                    // TODO P2 (no engine call site)
+ (UIImage *)imageWithContentsOfFile:(NSString *)path;       // P4: link-only (loadGenFromDisk, dead)
// Both take ownership of `cgImage` (they do NOT copy).
//
// USE -initWithCGImage: ON THE TEXTURE-LOAD PATH, NOT THE AUTORELEASED CONVENIENCE. Resources::
// loadResources runs during World construction, i.e. before the frame loop and therefore before
// any NSAutoreleasePool exists — +[NSAutoreleasePool currentPool] answers that by lazily creating
// a fallback root pool that is *never drained by design*, so an autoreleased image created there
// keeps its decoded pixels resident for the life of the tab. With ~120 textures loading through
// this path that is tens of MB, on the platform (iOS Safari, see audit row A11/D2) least able to
// spare it. The convenience form is correct for the RECOLOR path, which only ever runs inside a
// frame, where audit row A2's per-frame pool drains it.
+ (UIImage *)imageWithCGImage:(CGImageRef)cgImage;
- (id)initWithCGImage:(CGImageRef)cgImage;
- (CGImageRef)CGImage;
- (UIImageOrientation)imageOrientation;
- (CGSize)size;
- (void)drawInRect:(CGRect)rect;                             // TODO P2 (no engine call site)
@end

// Phase N Stage 1 (WORKING/native-migration-plan-2026-09-04.md): UIFont is the ONE name in this
// file that a native macOS process already has — Apple's private UIFoundation.framework, which
// real Foundation loads, registers a class called `UIFont`. Two classes with one name is not a
// warning to live with: libobjc picks one arbitrarily ("One of the duplicates must be removed or
// renamed"), and if it picks theirs then every `[UIFont systemFontOfSize:]` in this port lands in
// code that knows nothing about `_pointSize`.
//
// `@compatibility_alias` is the exact tool for this: the CLASS is registered under a unique name,
// while the SOURCE spelling `UIFont` — which Classes/Resources.mm and Classes/statusbar.mm use and
// which is not ours to change — keeps resolving to it. Measured: UIFont was the only collision the
// runtime reported across this whole stub set.
#if defined(EDEN_USE_SYSTEM_FOUNDATION)
#define EDEN_UIFONT_CLASS EdenUIFont
#else
#define EDEN_UIFONT_CLASS UIFont
#endif

@interface EDEN_UIFONT_CLASS : NSObject {
    @public
    float _pointSize; // Texture2D_web.mm's initFromString needs the real size back out, since the
                       // engine only ever hands us the UIFont* it got from systemFontOfSize:.
}
+ (EDEN_UIFONT_CLASS *)systemFontOfSize:(float)size;
- (float)pointSize;
@end

// AFTER the @interface, necessarily: @compatibility_alias requires its target class to be
// declared already.
#if defined(EDEN_USE_SYSTEM_FOUNDATION)
@compatibility_alias UIFont EdenUIFont;
#endif

@interface EDEN_UIKIT_CLASS(UIColor) : NSObject
+ (UIColor *)colorWithRed:(float)r green:(float)g blue:(float)b alpha:(float)a; // TODO P2
@end

// UIView: only exists because Classes/EAGLView.h (kept unmodified, see archive/PORT-STATUS-2026-08-13.md "Design
// decision: seam .mm replacements") declares `@interface EAGLView : UIView <...>`. No engine
// (non-seam) file after VKeyboard.mm's reclassification (see foundation-usage.md) calls a real
// UIView method — this is here purely so that inheritance chain parses.
@interface EDEN_UIKIT_CLASS(UIView) : NSObject
- (void)insertSubview:(UIView *)view atIndex:(NSInteger)index; // TODO P2/P3: no-op; the one
                                                                 // real caller (VKeyboard.mm)
                                                                 // is now seam-excluded.
@end

@interface EDEN_UIKIT_CLASS(UIAccelerometer) : NSObject
+ (UIAccelerometer *)sharedAccelerometer; // TODO: no known engine call site reads this beyond
                                           // a declaration; verify during P1 if it matters.
@end

@protocol UITextFieldDelegate <NSObject>
@optional
@end

// ---- EAGLContext — needed only because Classes/EAGLView.h (kept unmodified — see
// archive/PORT-STATUS-2026-08-13.md "Design decision: seam .mm replacements") declares an `EAGLContext *context`
// ivar/property. Real behavior (if any is even needed) belongs to Stage P2's WebGL2 context
// setup (src/seam/EAGLView_web.*) — this is just enough for the untouched original header to
// parse for the 3 non-seam engine files that still `#import "EAGLView.h"` (Globals.mm,
// Util.mm, World.mm — all three only ever `extern`/define the `G_EAGL_VIEW` pointer, never
// call a method through it, see foundation-usage.md).
typedef enum { kEAGLRenderingAPIOpenGLES1 = 1, kEAGLRenderingAPIOpenGLES2 = 2 } EAGLRenderingAPI;

@interface EDEN_UIKIT_CLASS(EAGLContext) : NSObject
+ (EAGLContext *)currentContext;
+ (BOOL)setCurrentContext:(EAGLContext *)context;
- (id)initWithAPI:(EAGLRenderingAPI)api;
@end

#endif
