// Trampoline for `#import <UIKit/UIKit.h>` — see Foundation/Foundation.h in this same
// framework/ dir for the angle-bracket-redirection rationale. Pulls in uikit_stubs.h (see
// that file's header comment for what's real — CGPoint/CGRect/CGSize/UITouch/UIEvent — vs.
// P2-deferred-opaque — UIImage/UIFont/UIColor/UIView/UIAccelerometer).
#ifndef EDEN_TRAMPOLINE_UIKIT_H
#define EDEN_TRAMPOLINE_UIKIT_H

#include "../../NSObject.h"     // UIKit re-exports Foundation on real iOS; match that.
#include "../../uikit_stubs.h"

// Real iOS's <UIKit/UIKit.h> transitively pulls in <QuartzCore/QuartzCore.h> ->
// <QuartzCore/CAEAGLLayer.h> -> <OpenGLES/EAGL.h> -> <OpenGLES/gltypes.h>, which is why engine
// headers that #import "Globals.h" (declaring `const GLubyte blockColor[...]`) BEFORE they
// #import <OpenGLES/ES1/gl.h> still compile on-device — GLubyte is already in scope from the
// prefix header's UIKit import. Match that transitive pull here rather than editing engine
// include order (CLAUDE.md: don't touch engine sources).
#include <OpenGLES/ES1/gl.h>

#endif
