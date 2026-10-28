// IOSKeyboard_native.mm — where the iOS system keyboard is (ROADMAP C.4, dev-console-plan.md §4.3).
//
// SDL3 has no keyboard-frame API: its UIKit controller watches the keyboard only to slide the whole
// view up until the text-input area clears it. The Console instead lays itself out ABOVE the
// keyboard (Classes/ConsoleMenu.mm), so it needs the frame itself. This is that observer, behind a
// plain-C wall read by Input_native.cpp's eden_text_input_keyboard_top().
//
// Compiled as its own object library with the REAL UIKit headers and ARC, iOS only
// (native/CMakeLists.txt, next to eden_apple_net, for the same reason: the main target's include
// path shadows <UIKit/UIKit.h> with this port's stubs).
//
// What it reports: how many points of the screen's BOTTOM the docked keyboard covers. A floating or
// split iPad keyboard (its frame does not reach the bottom edge) and a hidden one report 0. The SDL
// window is the full screen on iOS, so screen points are window points. Main thread only (UIKit
// posts keyboard notifications there, and the engine reads on the main thread).
#include <UIKit/UIKit.h>

static float    s_cover  = 0.0f;
static unsigned s_serial = 0;
static bool     s_installed = false;

static void cover_from(NSNotification* note) {
    NSValue* v = note.userInfo[UIKeyboardFrameEndUserInfoKey];
    const CGRect screen = [UIScreen mainScreen].bounds;
    float cover = 0.0f;
    if (v) {
        const CGRect kb = [v CGRectValue];
        const bool docked = CGRectGetMaxY(kb) >= CGRectGetMaxY(screen) - 1.0 && kb.size.height > 0.0 &&
                            CGRectIntersectsRect(kb, screen);
        if (docked) cover = (float)(CGRectGetMaxY(screen) - CGRectGetMinY(kb));
    }
    s_cover = cover > 0.0f ? cover : 0.0f;
    s_serial++;
}

static void install() {
    if (s_installed) return;
    s_installed = true;
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    [nc addObserverForName:UIKeyboardWillChangeFrameNotification object:nil queue:nil
                usingBlock:^(NSNotification* note) { cover_from(note); }];
    [nc addObserverForName:UIKeyboardWillHideNotification object:nil queue:nil
                usingBlock:^(NSNotification* note) { (void)note; s_cover = 0.0f; s_serial++; }];
}

// Installed lazily: the first caller is eden_text_input_start*, before it raises the keyboard.
extern "C" float eden_ios_keyboard_cover(void)     { install(); return s_cover; }
extern "C" unsigned eden_ios_keyboard_serial(void) { install(); return s_serial; }
