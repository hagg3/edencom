// WindowState_native.cpp -- UX.5 (WORKING/ui-audit-2026-11-11 B8, U3, I8): the desktop window's
// basics. A minimum size, fullscreen (Settings > Video > Fullscreen, F11) and the window's
// size/position remembered across launches. iOS has no window manager: every call here is a no-op
// there (and `eden_host_can_quit()` is 0), so the shared callers need no #if.
//
// Prefs keys are `window.x/y/w/h`; fullscreen is the ordinary `fullscreen` setting row, so it
// persists with the other settings. The pure half (eden_window_prefs_*, eden_window_size_ok) needs
// no window, which is what --base-selftest checks.

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>

#include "../shim/foundation/eden_prefs.h"
#include "DisplayProfile.h"

#include "eden_settings.h"   // eden_settings_index_of / eden_settings_set (QA.19)
extern "C" {

SDL_Window* eden_native_gl_window(void);

enum { kMinW = 568, kMinH = 320 };
static const float kMinAspect = 1.2f;

#if defined(EDEN_PLATFORM_IOS)
static const bool kDesktop = false;
#else
static const bool kDesktop = true;
#endif

static bool g_fsWanted = false;   // the setting's value; applied as soon as a window exists

// A saved size is believed only if it is a sane landscape window (a narrower one letterboxes, so a
// saved portrait size falls back to the default rather than being restored).
int eden_window_size_ok(int w, int h) {
    return w >= kMinW && h >= kMinH && w <= 16384 && h <= 16384 && (float)w >= kMinAspect * (float)h;
}

void eden_window_prefs_save(int x, int y, int w, int h) {
    eden_prefs_set_int("window.x", x);
    eden_prefs_set_int("window.y", y);
    eden_prefs_set_int("window.w", w);
    eden_prefs_set_int("window.h", h);
}

// 1 and the saved rectangle when one is stored and its size passes eden_window_size_ok.
int eden_window_prefs_load(int* x, int* y, int* w, int* h) {
    if (eden_prefs_type("window.w") != 1 || eden_prefs_type("window.h") != 1) return 0;
    const int ww = eden_prefs_get_int("window.w", 0), hh = eden_prefs_get_int("window.h", 0);
    if (!eden_window_size_ok(ww, hh)) return 0;
    *w = ww; *h = hh;
    *x = eden_prefs_get_int("window.x", SDL_WINDOWPOS_UNDEFINED);
    *y = eden_prefs_get_int("window.y", SDL_WINDOWPOS_UNDEFINED);
    return 1;
}

int eden_host_can_quit(void) { return kDesktop ? 1 : 0; }

// UX.5 (U3): the home Quit tile -- a desktop window with a mouse/keyboard profile. A phone-shaped
// preview (--device=iphone16) runs the touch profile and shows none.
int eden_home_quit_offered(void) { return kDesktop && eden_active_profile() == EDEN_PROFILE_DESKTOP ? 1 : 0; }

void eden_host_quit(void) {
    if (!kDesktop) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&e);
}

// Right after the window exists (main.cpp): the limits, and the remembered position.
void eden_window_boot(int restorePosition) {
    SDL_Window* win = eden_native_gl_window();
    if (!kDesktop || !win) return;
    SDL_SetWindowMinimumSize(win, kMinW, kMinH);
    // No SDL_SetWindowAspectRatio: on macOS (SDL 3.4.12) a min != max ratio goes through SDL's
    // windowWillResize:, and leaving fullscreen then traps in AppKit (-[NSWindow
    // _adjustNeedsDisplayRegionForNewFrame:], SIGTRAP; repro TESTERS/fs-exit-crash/repro.c). A window
    // narrower than 1.2:1 is allowed; DisplayProfile clamps the layout aspect and letterboxes it.
    int x, y, w, h;
    if (restorePosition && eden_window_prefs_load(&x, &y, &w, &h)) {
        SDL_Point p = {x + w / 2, y + h / 2};
        if (SDL_GetDisplayForPoint(&p)) SDL_SetWindowPosition(win, x, y);   // only if still on a screen
    }
}

// The Fullscreen setting's value, from Settings (any time) and the boot (App.cpp, once the window exists).
void eden_window_set_fullscreen(int on) {
    g_fsWanted = on != 0;
    SDL_Window* win = eden_native_gl_window();
    if (!kDesktop || !win) return;
    if (((SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) != 0) != g_fsWanted)
        SDL_SetWindowFullscreen(win, g_fsWanted);
}
// F11: flips the Fullscreen setting (which persists it and lands in eden_window_set_fullscreen).
void eden_window_fullscreen_toggle(void) {
    const int i = eden_settings_index_of("fullscreen");
    if (i >= 0) eden_settings_set(i, g_fsWanted ? 0.0f : 1.0f);
    else eden_window_set_fullscreen(!g_fsWanted);
}

// The OS changed it (the green button, Alt+Enter): keep the setting true to the window.
void eden_window_fullscreen_changed(int on) {
    if ((on != 0) == g_fsWanted) return;
    const int i = eden_settings_index_of("fullscreen");
    if (i >= 0) eden_settings_set(i, on ? 1.0f : 0.0f);
    else g_fsWanted = on != 0;
}
int  eden_window_fullscreen_wanted(void) { return g_fsWanted ? 1 : 0; }
void eden_window_apply_pending(void) { eden_window_set_fullscreen(g_fsWanted); }

// At quit: windowed rectangle only (a fullscreen window's rectangle is the display's).
void eden_window_remember(void) {
    SDL_Window* win = eden_native_gl_window();
    if (!kDesktop || !win || (SDL_GetWindowFlags(win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED))) return;
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(win, &x, &y);
    SDL_GetWindowSize(win, &w, &h);
    if (eden_window_size_ok(w, h)) eden_window_prefs_save(x, y, w, h);
}

}  // extern "C"
