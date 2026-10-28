// App.h — the process-lifetime app object: boot, frame, lifecycle (SM.9). Replaces the four-file
// chain main_native -> EdenAppDelegate_native -> EdenViewController_native -> SDLView_native, which
// mirrored iOS's UIApplicationMain -> app delegate -> view controller -> EAGLView. Everything is in
// App.cpp, in boot order. port/src/harness/ owns `int main` and the poll loop that calls drawFrame().
#ifndef EDEN_SEAM_APP_H
#define EDEN_SEAM_APP_H

class World;

namespace eden_native {

class App {
public:
    // Boot: publish the screen metrics, build the World, start the loop. Once; later calls return.
    // Needs the GL context live first (port/src/harness/main.cpp).
    static void boot();
    static App* get();   // null before boot()

    // One frame. Returns what World::update returned (the retina-swap-requested flag).
    bool drawFrame(bool renderThisFrame = true);

    void startAnimation();
    void stopAnimation();
    bool isAnimating() const { return animating; }

    // Lifecycle, driven by the SDL events in port/src/harness/core.cpp:
    //   SDL_EVENT_WINDOW_FOCUS_LOST   -> onVisibilityHidden()  (stop the loop; does NOT save)
    //   SDL_EVENT_WINDOW_FOCUS_GAINED -> onVisibilityVisible()
    //   SDL_EVENT_QUIT / WILL_ENTER_BACKGROUND (iOS) -> onPageHide(): save the open world, then stop.
    void onVisibilityHidden();
    void onVisibilityVisible();
    void onPageHide();

    // Force a FIXED etime instead of reading the clock. Zero (the default) means real elapsed time,
    // which an interactive session must use. Scripted harnesses set 1/60 s so that N frames is
    // always exactly N/60 s of engine time (the wall clock made "hold W for 300 frames" travel
    // anywhere from 0.00 to 92.31 blocks on the same binary).
    void setFixedEtime(float seconds) { fixedEtime = seconds; }

    World* world = nullptr;

private:
    bool animating = false;
    double startTime = 0.0;   // seconds, monotonic (eden_platform_now_ms())
    double lastTime = 0.0;
    float  fixedEtime = 0.0f; // 0 = use the clock
};

}  // namespace eden_native

#endif
