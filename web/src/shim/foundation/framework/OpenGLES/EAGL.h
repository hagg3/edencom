// Trampoline for `#import <OpenGLES/EAGL.h>` (angle-bracket, referenced by the UNMODIFIED
// original Classes/EAGLView.h / Classes/EdenViewController.h — see archive/PORT-STATUS-2026-08-13.md "Design
// decision: seam .mm replacements"). EAGLContext itself is declared in uikit_stubs.h (it's
// UIKit-adjacent glue more than a GL call, and EAGLView.h needs both UIView and EAGLContext
// from the same conceptual shim layer).
#ifndef EDEN_TRAMPOLINE_OPENGLES_EAGL_H
#define EDEN_TRAMPOLINE_OPENGLES_EAGL_H
#include "../../uikit_stubs.h"
#endif
